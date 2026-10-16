// TaskNest Capture - shared capture logic.
//
// A classic script (no modules, no DOM access) so that the popup loads it with
// <script src>, the service worker with importScripts(), and the QML tests
// import it directly. Every function is pure except sendCapture(), which takes
// the messaging function as a parameter.
//
// Page titles, addresses and selected text are untrusted input: nothing here
// builds HTML, and the UI code only ever uses textContent.

var TASKNEST_HOST = "com.tasknest.host";
var MAX_TEXT = 10000;
var MAX_TITLE = 500;
var MAX_URL = 2048;
var MAX_AUTO_TITLE = 80;

// Collapses whitespace and control characters; used for titles.
function oneLine(value) {
    return String(value === undefined || value === null ? "" : value)
        .replace(/[\u0000-\u001f\u007f]+/g, " ")
        .replace(/\s+/g, " ")
        .trim();
}

// Selected text: keeps line breaks and tabs, drops other control characters.
function cleanText(value) {
    return String(value === undefined || value === null ? "" : value)
        .replace(/\r\n?/g, "\n")
        .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "")
        .trim();
}

// A sensible task title from the selection: its first non-empty line.
function deriveTitle(text) {
    var lines = cleanText(text).split("\n");
    var first = "";
    for (var i = 0; i < lines.length; ++i) {
        first = oneLine(lines[i]);
        if (first !== "")
            break;
    }
    if (first.length > MAX_AUTO_TITLE)
        first = first.slice(0, MAX_AUTO_TITLE - 1).trim() + "…";
    return first;
}

// Only ordinary web pages can be a source.
function checkUrl(url) {
    var value = String(url === undefined || url === null ? "" : url).trim();
    if (value === "")
        return { ok: false, error: "This page has no address that TaskNest can store." };
    if (value.length > MAX_URL)
        return { ok: false, error: "The page address is longer than " + MAX_URL + " characters." };
    if (/\s/.test(value))
        return { ok: false, error: "The page address contains spaces." };
    var match = /^(https?):\/\/[^\/?#]+/i.exec(value);
    if (!match)
        return { ok: false, error: "Only http and https pages can be captured." };
    return { ok: true, value: value };
}

// Everything the popup, the context menu and the tests agree on.
// capture = { title, text, url }  ->  { ok, error?, value? }
function validateCapture(capture) {
    var text = cleanText(capture.text);
    if (text === "")
        return { ok: false, error: "Select some text on the page first." };
    if (text.length > MAX_TEXT)
        return { ok: false, error: "The selection is " + text.length + " characters; the limit is "
                                   + MAX_TEXT + ". Select less text." };
    var url = checkUrl(capture.url);
    if (!url.ok)
        return url;
    var title = oneLine(capture.title);
    if (title === "")
        title = deriveTitle(text);
    if (title === "")
        return { ok: false, error: "Enter a title." };
    if (title.length > MAX_TITLE)
        return { ok: false, error: "The title is longer than " + MAX_TITLE + " characters." };
    return { ok: true, value: { title: title, text: text, url: url.value } };
}

// A fresh id for one capture (the host accepts 8-64 of A-Z a-z 0-9 - _). It is
// never empty: an empty id would make a retry indistinguishable from a new capture.
function newRequestId() {
    var c = (typeof self !== "undefined" && self.crypto) ? self.crypto : null;
    if (c && typeof c.randomUUID === "function")
        return c.randomUUID().replace(/-/g, "");
    var hex = "";
    if (c && typeof c.getRandomValues === "function") {
        var bytes = c.getRandomValues(new Uint8Array(16));
        for (var i = 0; i < bytes.length; ++i)
            hex += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
        return hex;
    }
    for (var j = 0; j < 32; ++j)
        hex += Math.floor(Math.random() * 16).toString(16);
    return hex;
}

// The exact message sent to the native host. Only these four fields exist.
function buildRequest(value, requestId) {
    var message = { op: "capture", title: value.title, url: value.url, text: value.text };
    if (requestId)
        message.request_id = requestId;
    return message;
}

// Advice for each error code the native host can return, or for the browser
// failing to start it at all.
function describeFailure(error) {
    var code = error && error.code ? String(error.code) : "";
    switch (code) {
    case "integration_disabled":
        return "TaskNest is running, but its local integration API is off. In TaskNest open "
               + "Settings and turn on “Local integration API”, then capture again.";
    case "not_running":
        return "TaskNest is not running. Start TaskNest and capture again.";
    case "unauthorized":
        return "TaskNest refused the request. Capture again; if it keeps failing, turn the "
               + "integration off and on in TaskNest Settings.";
    case "timeout":
        return "TaskNest did not answer in time. Try again in a moment.";
    case "outcome_unknown":
        return "TaskNest did not confirm the capture, so it may or may not have been saved. "
               + "Check TaskNest to see whether it was saved. In the popup, “Check again” "
               + "retries safely: the same capture is never saved twice.";
    case "too_large":
    case "invalid_argument":
        return error.message ? String(error.message) : "TaskNest could not accept this capture.";
    case "host_missing":
        return "The TaskNest browser bridge is not installed. In TaskNest Settings turn on "
               + "“Browser capture”, then reload this page.";
    case "rejected":
        return "TaskNest could not store the capture.";
    default:
        return "Capture failed. Check that TaskNest is running with its integration API enabled.";
    }
}

// The longest the extension waits for the host. The host answers within about
// 9 s at the very most and exits by itself after 12 s, so this only matters if
// the browser never settles the call; it is a bound, not an expected duration.
var CAPTURE_WAIT_MS = 20000;

// Outcome of one attempt: "stored" (the task exists), "failed" (TaskNest
// definitely did not store it: a retry is an ordinary retry) or "unknown" (the
// request may have reached TaskNest; only a retry with the SAME request id is
// safe, because TaskNest returns the original task instead of a second one).
function outcomeOf(result) {
    if (result.ok)
        return "stored";
    return result.code === "outcome_unknown" ? "unknown" : "failed";
}

// The browser's own wording for "no such host / not allowed" means the message
// never reached anything. Every other rejection (host exited, pipe closed,
// communication error) happened after the host may have started work.
function hostNeverReached(failure) {
    var text = failure && failure.message !== undefined ? failure.message : failure;
    return /not found|forbidden|not allowed/i.test(String(text === undefined ? "" : text));
}

// sendNative(hostName, message) -> Promise<reply>  (chrome.runtime.sendNativeMessage)
// Resolves to { ok:true, taskId, duplicate, outcome } or { ok:false, code, message, outcome }.
// options: { timeoutMs, setTimer(fn, ms) -> handle, clearTimer(handle) } (tests inject these)
function sendCapture(sendNative, value, requestId, options) {
    var request = buildRequest(value, requestId);
    var opts = options || {};
    var arm = opts.setTimer || (typeof setTimeout === "function" ? setTimeout : null);
    var disarm = opts.clearTimer || (typeof clearTimeout === "function" ? clearTimeout : null);
    var waitMs = opts.timeoutMs === undefined ? CAPTURE_WAIT_MS : opts.timeoutMs;

    function finish(result) {
        result.outcome = outcomeOf(result);
        return result;
    }
    var attempt = sendNative(TASKNEST_HOST, request).then(function (reply) {
        if (!reply || typeof reply !== "object")
            return finish({ ok: false, code: "outcome_unknown",
                            message: describeFailure({ code: "outcome_unknown" }) });
        if (reply.ok === true) {
            var result = reply.result || {};
            return finish({ ok: true, taskId: result.task_id, duplicate: result.duplicate === true });
        }
        var error = reply.error || {};
        return finish({ ok: false, code: String(error.code || "unknown"), message: describeFailure(error) });
    }, function (failure) {
        // The browser's own wording is not shown.
        var code = hostNeverReached(failure) ? "host_missing" : "outcome_unknown";
        return finish({ ok: false, code: code, message: describeFailure({ code: code }) });
    });
    if (!arm || !(waitMs > 0))
        return attempt;
    // First of "answer" and "time is up" wins. A hand-rolled thenable keeps this
    // file free of Promise (the QML tests run it without one).
    var settled = false;
    var waiting = [];
    var value = null;
    function settle(result) {
        if (settled)
            return;
        settled = true;
        value = result;
        if (disarm && timer !== undefined)
            disarm(timer);
        waiting.forEach(function (fn) { fn(result); });
    }
    var timer = arm(function () {
        settle(finish({ ok: false, code: "outcome_unknown",
                        message: describeFailure({ code: "outcome_unknown" }) }));
    }, waitMs);
    attempt.then(settle);
    return {
        then: function (onResult) {
            if (settled)
                onResult(value);
            else
                waiting.push(onResult);
            return this;
        }
    };
}

// One capture as the user sees it (one popup, or one context-menu click).
// The request id belongs to the capture, not to the attempt: pressing the
// button again after an unknown outcome re-sends the same id, and a second
// press while an attempt is pending is ignored.
// state: "idle" | "pending" | "stored" | "unknown" | "failed"
function createCaptureSession(sendNative, requestId, options) {
    var session = {
        requestId: requestId,
        state: "idle",
        attempts: 0,
        ignored: 0,
        last: null,
        // Returns a promise of the attempt's result, or null when nothing was sent.
        submit: function (value) {
            if (session.state === "pending" || session.state === "stored") {
                session.ignored += 1;
                return null;
            }
            session.state = "pending";
            session.attempts += 1;
            return sendCapture(sendNative, value, session.requestId, options).then(function (result) {
                session.last = result;
                session.state = result.outcome;
                return result;
            });
        }
    };
    return session;
}
