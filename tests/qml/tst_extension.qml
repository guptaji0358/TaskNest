import QtQuick
import QtTest
import "../../browser-extension/capture-core.js" as Core

// Tests the browser extension's shared capture logic (capture-core.js) with a
// fake messaging function: validation, titles, request shape, error mapping.
TestCase {
    name: "ExtensionCore"

    // A synchronous stand-in for the Promise chrome.runtime.sendNativeMessage returns.
    function thenable(value, rejected) {
        return {
            then: function (onOk, onFail) {
                return thenable(rejected ? onFail(value) : onOk(value), false)
            }
        }
    }

    function test_validCaptureKeepsUnicodeMultilineAndHtmlLikeText() {
        const text = "Caf\u00e9 \u4e2d\u6587 \ud83d\ude00\n<b>bold?</b> & <script>alert(1)</script>\r\nthird\tline"
        const r = Core.validateCapture({ title: "", text: text, url: "https://example.com/a?b=c#d" })
        verify(r.ok)
        compare(r.value.text, "Caf\u00e9 \u4e2d\u6587 \ud83d\ude00\n<b>bold?</b> & <script>alert(1)</script>\nthird\tline")
        compare(r.value.title, "Caf\u00e9 \u4e2d\u6587 \ud83d\ude00")
        compare(r.value.url, "https://example.com/a?b=c#d")
    }

    function test_emptyAndWhitespaceSelectionsAreRefused() {
        const values = ["", "   ", "\n\t \r\n", undefined, null]
        for (let i = 0; i < values.length; ++i) {
            const r = Core.validateCapture({ title: "x", text: values[i], url: "https://example.com/" })
            verify(!r.ok)
            verify(r.error.length > 0)
        }
    }

    function test_onlyWebAddressesAreAccepted() {
        const bad = ["javascript:alert(1)", "file:///C:/x", "chrome://settings", "about:blank",
                     "ftp://example.com/x", "data:text/html,hi", "https://", "https:///x",
                     "not a url", "", undefined, "https://exa mple.com/",
                     "https://example.com/" + "p".repeat(2100)]
        for (let i = 0; i < bad.length; ++i)
            verify(!Core.validateCapture({ title: "t", text: "hello", url: bad[i] }).ok, String(bad[i]).slice(0, 40))
        verify(Core.validateCapture({ title: "t", text: "hello", url: "http://localhost:8080/x" }).ok)
        verify(Core.validateCapture({ title: "t", text: "hello", url: "HTTPS://EXAMPLE.COM/" }).ok)
    }

    function test_limitsOnTextAndTitle() {
        verify(Core.validateCapture({ title: "", text: "x".repeat(10000), url: "https://a.b/" }).ok)
        const tooLong = Core.validateCapture({ title: "", text: "x".repeat(10001), url: "https://a.b/" })
        verify(!tooLong.ok)
        verify(tooLong.error.indexOf("10000") >= 0)
        verify(Core.validateCapture({ title: "t".repeat(500), text: "a", url: "https://a.b/" }).ok)
        verify(!Core.validateCapture({ title: "t".repeat(501), text: "a", url: "https://a.b/" }).ok)
    }

    function test_autoTitleIsTheFirstNonEmptyLineAndIsShort() {
        compare(Core.deriveTitle("\n\n  Hello   world  \nsecond"), "Hello world")
        const long = Core.deriveTitle("w".repeat(200))
        verify(long.length <= 80)
        verify(long.endsWith("\u2026"))
        compare(Core.deriveTitle("   "), "")
        // an edited title wins and is collapsed to one line
        const r = Core.validateCapture({ title: "  My\n  title ", text: "body", url: "https://a.b/" })
        compare(r.value.title, "My title")
    }

    function test_requestHasOnlyTheDocumentedFieldsAndNeverAToken() {
        const v = Core.validateCapture({ title: "T", text: "body", url: "https://a.b/" }).value
        const req = Core.buildRequest(v, "abc12345def")
        compare(Object.keys(req).sort().join(","), "op,request_id,text,title,url")
        compare(req.op, "capture")
        const bare = Core.buildRequest(v, "")
        verify(!("request_id" in bare))
        verify(JSON.stringify(req).indexOf("token") < 0)
    }

    function test_successAndDuplicateReplies() {
        let sent = null
        const v = Core.validateCapture({ title: "T", text: "body", url: "https://a.b/" }).value
        let result = null
        Core.sendCapture(function (host, message) {
            sent = { host: host, message: message }
            return thenable({ ok: true, result: { task_id: 7 } })
        }, v, "req-00000001").then(function (r) { result = r })
        compare(sent.host, "com.tasknest.host")
        compare(sent.message.request_id, "req-00000001")
        verify(result.ok)
        compare(result.taskId, 7)
        compare(result.duplicate, false)

        Core.sendCapture(function () {
            return thenable({ ok: true, result: { task_id: 7, duplicate: true } })
        }, v, "req-00000001").then(function (r) { result = r })
        compare(result.duplicate, true)
    }

    function test_everyHostErrorHasAnActionableMessage() {
        const v = Core.validateCapture({ title: "T", text: "body", url: "https://a.b/" }).value
        const cases = {
            integration_disabled: "Local integration API",
            not_running: "not running",
            unauthorized: "refused",
            timeout: "did not answer",
            invalid_argument: "bad value",
            too_large: "bad value",
            host_missing: "Browser capture",
            rejected: "could not store",
            something_new: "Capture failed"
        }
        Object.keys(cases).forEach(function (code) {
            let got = null
            Core.sendCapture(function () {
                return thenable({ ok: false, error: { code: code, message: "bad value" } })
            }, v, "r").then(function (r) { got = r })
            verify(!got.ok, code)
            compare(got.code, code)
            verify(got.message.indexOf(cases[code]) >= 0, code + ": " + got.message)
        })
        // the browser failing to reach the host never leaks its own text
        let failed = null
        Core.sendCapture(function () { return thenable("Specified native messaging host not found.", true) },
                         v, "r").then(function (r) { failed = r })
        verify(!failed.ok)
        compare(failed.code, "host_missing")
        verify(failed.message.indexOf("Specified native") < 0)
        // a malformed reply is an error, not a success, and not a claim that nothing was stored
        let malformed = null
        Core.sendCapture(function () { return thenable("surprise") }, v, "r").then(function (r) { malformed = r })
        verify(!malformed.ok)
        compare(malformed.outcome, "unknown")
    }

    // ---- timeouts and unknown outcomes ---------------------------------------

    // A reply that is delivered later, by the test, like the real browser call.
    function deferred() {
        const d = { done: false, rejected: false, value: undefined, children: [] }
        function run(child) {
            const handler = d.rejected ? child.onFail : child.onOk
            if (handler)
                child.next.settle(false, handler(d.value))
            else
                child.next.settle(d.rejected, d.value)
        }
        d.settle = function (rejected, value) {
            if (d.done)
                return
            d.done = true
            d.rejected = rejected
            d.value = value
            d.children.forEach(run)
        }
        d.then = function (onOk, onFail) {
            const child = { onOk: onOk, onFail: onFail, next: deferred() }
            d.children.push(child)
            if (d.done)
                run(child)
            return child.next
        }
        return d
    }
    function pendingCall() {
        const call = { waiting: [], sent: [] }
        call.send = function (host, message) {
            call.sent.push(message)
            const d = deferred()
            call.waiting.push(d)
            return d
        }
        call.reply = function (value) { call.waiting.shift().settle(false, value) }
        call.reject = function (value) { call.waiting.shift().settle(true, value) }
        return call
    }
    function captureValue() {
        return Core.validateCapture({ title: "T", text: "body", url: "https://a.b/" }).value
    }

    function test_hostUnknownOutcomeIsNotReportedAsAFailure() {
        const v = captureValue()
        let got = null
        Core.sendCapture(function () {
            return thenable({ ok: false, error: { code: "outcome_unknown", message: "x" } })
        }, v, "req-00000001").then(function (r) { got = r })
        verify(!got.ok)
        compare(got.outcome, "unknown")
        verify(got.message.indexOf("may or may not have been saved") >= 0, got.message)
        verify(got.message.indexOf("never saved twice") >= 0)
        verify(got.message.toLowerCase().indexOf("failed") < 0)

        // confirmed failures keep saying so, and never claim the capture may exist
        const confirmed = ["integration_disabled", "not_running", "unauthorized", "timeout",
                           "invalid_argument", "too_large", "rejected", "unsupported_operation"]
        confirmed.forEach(function (code) {
            let r = null
            Core.sendCapture(function () {
                return thenable({ ok: false, error: { code: code, message: "m" } })
            }, v, "req-00000001").then(function (x) { r = x })
            compare(r.outcome, "failed", code)
            verify(r.message.indexOf("may or may not") < 0, code)
        })
    }

    function test_browserRejectionAfterTheMessageWasSentIsUnknown() {
        const v = captureValue()
        const afterSend = ["Native host has exited.", "Error when communicating with the native messaging host.",
                           "Native host has exited", "something unexpected"]
        afterSend.forEach(function (text) {
            let r = null
            Core.sendCapture(function () { return thenable({ message: text }, true) }, v, "req-00000001")
                .then(function (x) { r = x })
            compare(r.outcome, "unknown", text)
            compare(r.code, "outcome_unknown", text)
        })
        const neverReached = ["Specified native messaging host not found.",
                              "Access to the specified native messaging host is forbidden."]
        neverReached.forEach(function (text) {
            let r = null
            Core.sendCapture(function () { return thenable({ message: text }, true) }, v, "req-00000001")
                .then(function (x) { r = x })
            compare(r.outcome, "failed", text)
            compare(r.code, "host_missing", text)
        })
    }

    function test_noAnswerWithinTheBoundIsUnknownAndALateAnswerIsIgnored() {
        const v = captureValue()
        const call = pendingCall()
        const timers = []
        let cleared = 0
        let got = null
        Core.sendCapture(call.send, v, "req-00000001", {
            timeoutMs: 20000,
            setTimer: function (fn, ms) { timers.push({ fn: fn, ms: ms }); return timers.length },
            clearTimer: function () { cleared += 1 }
        }).then(function (r) { got = r })
        compare(timers.length, 1)
        compare(timers[0].ms, 20000)
        verify(got === null)
        timers[0].fn()
        compare(got.outcome, "unknown")
        // the answer arrives after the bound: it must not rewrite what the user was told
        const first = got
        call.reply({ ok: true, result: { task_id: 3 } })
        verify(got === first)

        // an answer inside the bound cancels the timer
        cleared = 0
        const call2 = pendingCall()
        let got2 = null
        Core.sendCapture(call2.send, v, "req-00000002", {
            timeoutMs: 20000,
            setTimer: function () { return 9 },
            clearTimer: function (h) { cleared += h }
        }).then(function (r) { got2 = r })
        call2.reply({ ok: true, result: { task_id: 4 } })
        compare(got2.outcome, "stored")
        compare(cleared, 9)
    }

    function test_slowButSuccessfulCaptureStaysPendingThenStored() {
        const call = pendingCall()
        const session = Core.createCaptureSession(call.send, "req-00000001")
        let got = null
        session.submit(captureValue()).then(function (r) { got = r })
        compare(session.state, "pending")
        verify(got === null)
        call.reply({ ok: true, result: { task_id: 12 } })
        compare(session.state, "stored")
        compare(got.taskId, 12)
        compare(got.outcome, "stored")
    }

    function test_repeatedClicksWhilePendingSendOnce() {
        const call = pendingCall()
        const session = Core.createCaptureSession(call.send, "req-00000001")
        verify(session.submit(captureValue()) !== null)
        for (let i = 0; i < 4; ++i)
            verify(session.submit(captureValue()) === null)
        compare(call.sent.length, 1)
        compare(session.ignored, 4)
        call.reply({ ok: true, result: { task_id: 5 } })
        // once stored, further presses are ignored as well
        verify(session.submit(captureValue()) === null)
        compare(call.sent.length, 1)
    }

    function test_retryAfterUnknownReusesTheSameRequestId() {
        const call = pendingCall()
        const session = Core.createCaptureSession(call.send, "req-00000001")
        session.submit(captureValue())
        call.reject({ message: "Native host has exited." })
        compare(session.state, "unknown")
        // the button stays actionable; the retry carries the original id
        const retry = session.submit(captureValue())
        verify(retry !== null)
        compare(session.state, "pending")
        call.reply({ ok: true, result: { task_id: 7, duplicate: true } })
        compare(session.state, "stored")
        compare(call.sent.length, 2)
        compare(call.sent[0].request_id, "req-00000001")
        compare(call.sent[1].request_id, "req-00000001")
        compare(session.last.duplicate, true)
        compare(session.last.taskId, 7)
        compare(session.attempts, 2)
    }

    function test_retryAfterAConfirmedFailureAlsoKeepsTheId() {
        const call = pendingCall()
        const session = Core.createCaptureSession(call.send, "req-00000001")
        session.submit(captureValue())
        call.reply({ ok: false, error: { code: "not_running", message: "m" } })
        compare(session.state, "failed")
        session.submit(captureValue())
        compare(call.sent[1].request_id, "req-00000001")
    }

    function test_generatedRequestIdsAreValidForTheHostAndDistinct() {
        const a = Core.newRequestId()
        const b = Core.newRequestId()
        verify(/^[A-Za-z0-9_-]{8,64}$/.test(a), "id accepted by the host: " + a)
        verify(/^[A-Za-z0-9_-]{8,64}$/.test(b))
        verify(a !== b, "two captures never share an id")
    }

    function test_aNewCaptureGetsItsOwnRequestIdAndTask() {
        const call = pendingCall()
        const a = Core.createCaptureSession(call.send, "req-0000000a")
        const b = Core.createCaptureSession(call.send, "req-0000000b")
        a.submit(captureValue())
        b.submit(captureValue()) // same page, a new capture (a second popup)
        compare(call.sent.length, 2)
        verify(call.sent[0].request_id !== call.sent[1].request_id)
        compare(call.sent[0].url, call.sent[1].url)
        call.reply({ ok: true, result: { task_id: 1 } })
        call.reply({ ok: true, result: { task_id: 2 } })
        verify(a.last.taskId !== b.last.taskId)
        verify(!b.last.duplicate)
    }
}
