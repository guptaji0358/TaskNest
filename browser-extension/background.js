// Service worker: only the "Capture selection to TaskNest" context-menu item.
// It has no state that must survive the worker being stopped; every capture
// opens its own native-messaging connection through the shared core.
importScripts("capture-core.js");

var MENU_ID = "tasknest-capture-selection";
var pendingTabs = {};

chrome.runtime.onInstalled.addListener(function () {
    chrome.contextMenus.removeAll(function () {
        chrome.contextMenus.create({
            id: MENU_ID,
            title: "Capture selection to TaskNest",
            contexts: ["selection"]
        });
    });
});

function badge(tabId, text, color, title) {
    chrome.action.setBadgeText({ tabId: tabId, text: text });
    if (color)
        chrome.action.setBadgeBackgroundColor({ tabId: tabId, color: color });
    chrome.action.setTitle({ tabId: tabId, title: title });
    if (text) {
        setTimeout(function () {
            chrome.action.setBadgeText({ tabId: tabId, text: "" });
            chrome.action.setTitle({ tabId: tabId, title: "Capture selection to TaskNest" });
        }, 6000);
    }
}

chrome.contextMenus.onClicked.addListener(function (info, tab) {
    if (info.menuItemId !== MENU_ID || !tab || tab.id === undefined)
        return;
    if (pendingTabs[tab.id])
        return; // one capture per tab at a time
    var check = validateCapture({ title: "", text: info.selectionText, url: info.pageUrl || tab.url });
    if (!check.ok) {
        badge(tab.id, "!", "#c0392b", check.error);
        return;
    }
    pendingTabs[tab.id] = true;
    var requestId = newRequestId();
    sendCapture(function (host, message) { return chrome.runtime.sendNativeMessage(host, message); },
                check.value, requestId).then(function (result) {
        delete pendingTabs[tab.id];
        if (result.ok)
            badge(tab.id, "✓", "#1e8f5a", "Captured in TaskNest");
        else if (result.outcome === "unknown")
            badge(tab.id, "?", "#b7791f", result.message);
        else
            badge(tab.id, "!", "#c0392b", result.message);
    });
});
