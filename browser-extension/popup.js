// Popup: shows the selection of the active tab and sends it on request.
// Page-derived content is only ever set with textContent / value, never as markup.
(function () {
    "use strict";

    var statusEl = document.getElementById("status");
    var form = document.getElementById("form");
    var titleEl = document.getElementById("title");
    var previewEl = document.getElementById("preview");
    var countEl = document.getElementById("count");
    var pageEl = document.getElementById("page");
    var urlEl = document.getElementById("url");
    var captureBtn = document.getElementById("capture");

    var tab = null;
    var selection = "";
    // One id per opened popup: a retry of the same capture cannot create a
    // second task, while a new capture (new popup) always can.
    var requestId = newRequestId();
    var session = createCaptureSession(function (host, message) {
        return chrome.runtime.sendNativeMessage(host, message);
    }, requestId);
    var pending = false;
    var done = false;

    function show(kind, message) {
        statusEl.hidden = false;
        statusEl.className = kind || "";
        statusEl.textContent = message;
    }

    // Runs inside the page, only because the user opened this popup. It reads
    // the page's own selection, never form fields or their values.
    function readSelection() {
        var active = document.activeElement;
        var tag = active && active.tagName ? active.tagName.toUpperCase() : "";
        if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT")
            return { blocked: true, text: "" };
        var s = window.getSelection ? window.getSelection() : null;
        return { blocked: false, text: s ? String(s.toString()) : "" };
    }

    function update() {
        var count = cleanText(selection).length;
        countEl.textContent = count + " of " + MAX_TEXT + " characters";
        var check = validateCapture({ title: titleEl.value, text: selection, url: tab && tab.url });
        captureBtn.disabled = pending || done || !check.ok;
        if (!check.ok && !done && !pending && selection !== "")
            show("error", check.error);
        else if (check.ok && !done && !pending && session.state !== "unknown")
            statusEl.hidden = true;
    }

    function send() {
        if (pending || done)
            return;
        var check = validateCapture({ title: titleEl.value, text: selection, url: tab && tab.url });
        if (!check.ok) {
            show("error", check.error);
            return;
        }
        var attempt = session.submit(check.value);
        if (!attempt)
            return; // an attempt is already pending, or the capture is stored
        pending = true;
        captureBtn.disabled = true;
        show("", "Sending to TaskNest…");
        attempt.then(function (result) {
            pending = false;
            if (result.outcome === "stored") {
                done = true;
                show("ok", result.duplicate ? "Already captured in TaskNest." : "Captured in TaskNest.");
                captureBtn.textContent = "Captured";
            } else if (result.outcome === "unknown") {
                // May be stored: the same id is re-sent, so a retry cannot duplicate it.
                show("warn", result.message);
                titleEl.disabled = true; // a retry returns the first task, so edits would be ignored
                captureBtn.textContent = "Check again";
            } else {
                show("error", result.message);
            }
            captureBtn.disabled = pending || done;
        });
    }

    function init() {
        chrome.tabs.query({ active: true, currentWindow: true }, function (tabs) {
            tab = tabs && tabs[0];
            if (!tab || tab.id === undefined) {
                show("error", "There is no page to capture from.");
                return;
            }
            var urlCheck = checkUrl(tab.url);
            if (!urlCheck.ok) {
                show("error", urlCheck.error);
                return;
            }
            chrome.scripting.executeScript({ target: { tabId: tab.id }, func: readSelection })
                .then(function (results) {
                    var r = results && results[0] && results[0].result;
                    if (r && r.blocked) {
                        show("error", "The selection is in a form field. TaskNest does not capture "
                                      + "form fields; select text in the page itself.");
                    } else {
                        selection = r ? cleanText(r.text) : "";
                    }
                    form.hidden = false;
                    pageEl.textContent = oneLine(tab.title) || "(untitled page)";
                    urlEl.textContent = urlCheck.value;
                    previewEl.textContent = selection;
                    if (selection === "" && !(r && r.blocked))
                        show("", "Select some text on the page, then click the TaskNest icon again.");
                    titleEl.value = deriveTitle(selection);
                    update();
                    titleEl.focus();
                    titleEl.select();
                }, function () {
                    show("error", "This page cannot be read by extensions (for example a browser "
                                  + "settings page or the web store).");
                });
        });
    }

    titleEl.addEventListener("input", update);
    titleEl.addEventListener("keydown", function (e) {
        if (e.key === "Enter") {
            e.preventDefault();
            send();
        }
    });
    captureBtn.addEventListener("click", send);
    init();
})();
