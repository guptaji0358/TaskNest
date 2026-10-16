"""End-to-end test of the TaskNest browser extension in Playwright's Chromium.

Run:  python tests/e2e/run_e2e.py          (from the repository root)

Safety:
  * TaskNest runs only with --test-profile <fresh folder under %TEMP%>, offscreen.
    The live database (out/bin/data) is hashed before and after as a tripwire.
  * The browser is Playwright's own Chromium with a throw-away user-data-dir.
    Real Chrome/Edge keys are never written; they are snapshotted before/after.
  * The only registry write is HKCU\\Software\\Chromium\\NativeMessagingHosts\\
    com.tasknest.host (read by non-branded Chromium only). The run refuses to start
    if that value already exists and removes it in `finally`.
  * Extension files are staged into the scratch folder (same key/id, plus
    host_permissions for 127.0.0.1); the repository copy is untouched.
  * Children are tracked by PID (Job Object with kill-on-close for TaskNest);
    browser leftovers are found by the unique scratch path, never by name.
"""
import ctypes, hashlib, http.server, json, os, shutil, sqlite3, subprocess, sys, tempfile
import threading, time, winreg
from ctypes import wintypes
from pathlib import Path

import win32api, win32job

ROOT = Path(__file__).resolve().parents[2]
EXE = ROOT / "out" / "bin" / "TaskNest.exe"
HOST_EXE = ROOT / "out" / "bin" / "TaskNestHost.exe"
LIVE_DB = ROOT / "out" / "bin" / "data" / "tasknest.db"
EXT_SRC = ROOT / "browser-extension"
EXT_ID = "fghdblbielgdmgnmodfhdifeiggaglcj"
HOST_NAME = "com.tasknest.host"
QT_DIRS = [r"D:\Apps\Qt6\Tools\mingw1310_64\bin", r"D:\Apps\Qt6\6.11.1\mingw_64\bin"]
CHROMIUM_NM = r"Software\Chromium\NativeMessagingHosts"

results = []
latencies = []          # ms from the click to a confirmed result in the real popup
unknown_outcomes = []    # captures the popup could not confirm
UNKNOWN_TEXT = "may or may not have been saved"


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS  " if cond else "FAIL  ") + name + (("  -> " + str(detail)) if (detail and not cond) else ""),
          flush=True)


# ---------- safety snapshots ----------
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def reg_dump(path):
    out = {"exists": False}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as k:
            out["exists"] = True
            out["values"] = []
            i = 0
            while True:
                try:
                    out["values"].append(winreg.EnumValue(k, i)[:2])
                    i += 1
                except OSError:
                    break
            subs = []
            i = 0
            while True:
                try:
                    subs.append(winreg.EnumKey(k, i))
                    i += 1
                except OSError:
                    break
            out["subkeys"] = sorted(subs)
    except OSError:
        pass
    return out


WATCHED = [r"Software\Google\Chrome\NativeMessagingHosts", r"Software\Microsoft\Edge\NativeMessagingHosts",
           r"Software\Google\Chrome\NativeMessagingHosts" + "\\" + HOST_NAME,
           r"Software\Microsoft\Edge\NativeMessagingHosts" + "\\" + HOST_NAME,
           r"Software\TaskNestTestProfiles", CHROMIUM_NM]


def reg_snapshot():
    return {k: reg_dump(k) for k in WATCHED}


# ---------- process helpers ----------
class Thread32(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD), ("tpBasePri", wintypes.LONG),
                ("tpDeltaPri", wintypes.LONG), ("dwFlags", wintypes.DWORD)]


def post_quit(pid):
    """WM_QUIT to the threads of this child only (the offscreen app has no window to close)."""
    k = ctypes.windll.kernel32
    k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k.CreateToolhelp32Snapshot(4, 0)  # TH32CS_SNAPTHREAD
    e = Thread32()
    e.dwSize = ctypes.sizeof(e)
    posted = False
    ok = k.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            posted = bool(ctypes.windll.user32.PostThreadMessageW(e.th32ThreadID, 0x0012, 0, 0)) or posted
        ok = k.Thread32Next(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return posted


JOB = win32job.CreateJobObject(None, "")
_info = win32job.QueryInformationJobObject(JOB, win32job.JobObjectExtendedLimitInformation)
_info["BasicLimitInformation"]["LimitFlags"] |= win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
win32job.SetInformationJobObject(JOB, win32job.JobObjectExtendedLimitInformation, _info)


class App:
    def __init__(self, profile):
        self.profile = profile
        self.proc = None

    def start(self):
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        env["PATH"] = ";".join(QT_DIRS) + ";" + env["PATH"]
        self.proc = subprocess.Popen([str(EXE), "--test-profile", str(self.profile)], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        h = win32api.OpenProcess(0x1F0FFF, False, self.proc.pid)
        win32job.AssignProcessToJobObject(JOB, h)

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def quit(self):
        if not self.running():
            return True
        post_quit(self.proc.pid)
        try:
            self.proc.wait(15)
            return True
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
            return False


def wait_for(pred, timeout=30, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def kill_by_path_fragment(fragment):
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.CommandLine.Contains('%s') } "
          "| ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $_.ProcessId }"
          % fragment.replace("'", "''"))
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    return [x for x in r.stdout.split() if x.isdigit()]


# ---------- staging and deterministic cleanup ----------
TEMP_ROOT = Path(tempfile.gettempdir()).resolve()
SCRATCH_PREFIX = "tn-e2e-"


def stage_copy(src, dst, skip=("README.md",)):
    """Copy files only, with no timestamps or attributes.

    shutil.copytree copies directory attributes. The repository lives under OneDrive, whose
    placeholder folders are ReadOnly, and a read-only directory cannot be removed by
    shutil.rmtree (the cause of the 'leftover tn-e2e-* folder'; no process held it).
    """
    dst.mkdir(parents=True)
    for e in sorted(src.iterdir()):
        if e.name in skip:
            continue
        if e.is_dir():
            stage_copy(e, dst / e.name, skip=())
        else:
            shutil.copyfile(e, dst / e.name)


def is_harness_scratch(path):
    """Only a direct child of the temp root named tn-e2e-* may ever be deleted."""
    try:
        r = path.resolve()
    except OSError:
        return False
    return r.parent == TEMP_ROOT and r.name.startswith(SCRATCH_PREFIX) and r.is_dir() and not r.is_symlink()


def remove_scratch(scratch, log=print, attempts=5):
    """Delete the harness-created scratch folder. Returns (ok, [(path, reason)])."""
    import stat
    if not scratch.exists():
        return True, []
    if not is_harness_scratch(scratch):
        return False, [(str(scratch), "refused: not a direct tn-e2e-* child of %s" % TEMP_ROOT)]

    def on_error(func, path, exc):
        # identified cause 1: ReadOnly attribute (Access denied) -> clear it and retry once
        if isinstance(exc, PermissionError) and getattr(exc, "winerror", None) in (5, None):
            try:
                os.chmod(path, stat.S_IWRITE)
                log("cleanup: cleared read-only attribute on %s" % path)
                func(path)
                return
            except OSError as e2:
                exc = e2
        raise exc

    last = None
    for i in range(attempts):
        try:
            shutil.rmtree(scratch, onexc=on_error) if sys.version_info >= (3, 12) else                 shutil.rmtree(scratch, onerror=lambda f, p, ei: on_error(f, p, ei[1]))
            last = None
        except OSError as e:
            last = e
            # identified cause 2: a sharing violation (winerror 32) from a just-exited child
            if getattr(e, "winerror", None) == 32:
                log("cleanup: sharing violation on %s, retry %d/%d" % (e.filename, i + 1, attempts))
                time.sleep(1)
                continue
            break
        if not scratch.exists():
            return True, []
    remaining = [(str(f), "exists") for f in [scratch] + list(scratch.rglob("*"))] if scratch.exists() else []
    reason = repr(last) if last else "still present after removal"
    return not remaining, [(p, reason) for p, _ in remaining]


def host_status(bat):
    """Run the launcher exactly as a browser would and return the host's reply to {'op':'status'}."""
    import struct
    body = json.dumps({"op": "status"}).encode()
    r = subprocess.run(["cmd", "/c", str(bat)], input=struct.pack("<I", len(body)) + body,
                       capture_output=True, timeout=30)
    out = r.stdout
    if len(out) < 4:
        return {"raw": out[:80].decode("latin1"), "stderr": r.stderr[:200].decode("latin1")}
    n = struct.unpack("<I", out[:4])[0]
    return json.loads(out[4:4 + n])


# ---------- scratch db ----------
def db_rows(db):
    c = sqlite3.connect("file:%s?mode=ro" % db.as_posix(), uri=True)
    try:
        return c.execute("SELECT id,title,notes FROM tasks ORDER BY id").fetchall()
    finally:
        c.close()


def set_integration(db, value):
    c = sqlite3.connect(str(db))  # scratch profile database only
    try:
        c.execute("INSERT INTO app_metadata(key,value) VALUES('integration_enabled',?) "
                  "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (value,))
        c.commit()
    finally:
        c.close()


# ---------- fixture web server ----------
PAGE = (b"<!doctype html><title>Fixture page</title><body>"
        b'<p id="para">Buy oranges &lt;b&gt;literal&lt;/b&gt; and call Sam.</p>'
        b'<input id="inp" value="secret field text">')


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, *a):
        pass


BACKGROUND_PROBE = """async ([core, bg]) => {
    const calls = {created: [], removed: 0, sent: [], badges: []};
    const L = {};
    let reply;
    const stub = {
      contextMenus: {removeAll: cb => { calls.removed++; cb(); }, create: o => calls.created.push(o),
                     onClicked: {addListener: f => { L.click = f; }}},
      runtime: {onInstalled: {addListener: f => { L.inst = f; }},
                sendNativeMessage: (h, m) => { calls.sent.push([h, m]); return reply; }},
      action: {setBadgeText: o => calls.badges.push(o.text), setBadgeBackgroundColor: () => {}, setTitle: () => {}}};
    // one function scope for both scripts: `chrome` and `importScripts` are the mocks, the rest is the real code
    new Function('chrome', 'importScripts', core + '\\n' + bg)(stub, () => {});
    L.inst();
    const tab = {id: 7, url: 'http://127.0.0.1/x'};
    const ID = 'tasknest-capture-selection';
    reply = new Promise(r => setTimeout(() => r({ok: true, result: {task_id: 5}}), 30));
    L.click({menuItemId: 'other', selectionText: 'a', pageUrl: tab.url}, tab);
    const afterOther = calls.sent.length;
    L.click({menuItemId: ID, selectionText: '   ', pageUrl: tab.url}, tab);
    const afterEmpty = calls.sent.length;
    const badgesAfterEmpty = calls.badges.slice();
    L.click({menuItemId: ID, selectionText: 'hello world', pageUrl: tab.url}, tab);
    L.click({menuItemId: ID, selectionText: 'hello world', pageUrl: tab.url}, tab);
    const afterDouble = calls.sent.length;
    await new Promise(r => setTimeout(r, 150));
    const badgesAfterOk = calls.badges.slice();
    reply = Promise.resolve({ok: false, error: {code: 'not_running'}});
    L.click({menuItemId: ID, selectionText: 'again', pageUrl: tab.url}, tab);
    await new Promise(r => setTimeout(r, 100));
    return {calls, afterOther, afterEmpty, badgesAfterEmpty, afterDouble, badgesAfterOk, badgesFinal: calls.badges};
}"""


def main():
    for p in (EXE, HOST_EXE, EXT_SRC / "manifest.json"):
        if not p.exists():
            sys.exit("missing: %s" % p)
    before_db = sha(LIVE_DB)
    before_reg = reg_snapshot()
    if HOST_NAME in (before_reg[CHROMIUM_NM].get("subkeys") or []):
        sys.exit("refusing: %s already registered under HKCU\\%s" % (HOST_NAME, CHROMIUM_NM))

    scratch = Path(tempfile.mkdtemp(prefix="tn-e2e-")).resolve()
    profile = scratch / "profile"  # must not exist yet: TaskNest creates it and its marker
    assert str(scratch).lower().startswith(str(Path(tempfile.gettempdir()).resolve()).lower())
    app = App(profile)
    registered = False
    parent_created = not before_reg[CHROMIUM_NM]["exists"]
    server = None
    try:
        # 1. create the profile, seed the integration switch, restart
        app.start()
        marker = profile / "tasknest-test-profile.json"
        db = profile / "data" / "tasknest.db"
        check("queried database is the scratch one, not the live one",
              db.resolve() != LIVE_DB.resolve() and scratch in db.resolve().parents)
        check("scratch profile created with marker", wait_for(lambda: marker.exists() and db.exists()))
        time.sleep(1.5)
        check("first scratch launch closed gracefully", app.quit())
        set_integration(db, "1")
        app.start()
        disc = profile / "integration"
        check("integration endpoint up in scratch profile",
              wait_for(lambda: disc.exists() and list(disc.glob("integration-*.json")), 30))

        # 2. host launcher + manifest written by the harness, registered for Chromium only
        nh = profile / "native-host"
        nh.mkdir(exist_ok=True)
        bat = nh / (HOST_NAME + ".bat")
        bat.write_bytes(('@echo off\r\nset "PATH=%s;%%PATH%%"\r\n"%s" --test-profile "%s" %%*\r\n'
                         % (";".join(QT_DIRS), HOST_EXE, profile)).encode())
        manifest = nh / (HOST_NAME + ".json")
        manifest.write_text(json.dumps({"name": HOST_NAME, "description": "TaskNest e2e scratch host",
                                        "path": str(bat), "type": "stdio",
                                        "allowed_origins": ["chrome-extension://%s/" % EXT_ID]}, indent=2))
        with winreg.CreateKey(winreg.HKEY_CURRENT_USER, CHROMIUM_NM + "\\" + HOST_NAME) as k:
            winreg.SetValueEx(k, "", 0, winreg.REG_SZ, str(manifest))
        registered = True

        st = host_status(bat)
        for _ in range(15):  # the pipe can lag the discovery file by a moment
            if st.get("ok") is True:
                break
            time.sleep(1)
            st = host_status(bat)
        check("host (scratch profile) reports ready while scratch app runs",
              st.get("ok") is True and st.get("result", {}).get("state") == "ready", st)

        # 3. staged extension copy (same key => same id)
        stage = scratch / "extension"
        stage_copy(EXT_SRC, stage)
        m = json.loads((stage / "manifest.json").read_text(encoding="utf-8"))
        m["host_permissions"] = ["http://127.0.0.1/*"]
        (stage / "manifest.json").write_text(json.dumps(m, indent=2), encoding="utf-8")

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        base = "http://127.0.0.1:%d/page.html" % server.server_port

        from playwright.sync_api import sync_playwright
        with sync_playwright() as pw:
            ctx = pw.chromium.launch_persistent_context(
                str(scratch / "browser-data"), channel="chromium", headless=True,
                args=["--disable-extensions-except=" + str(stage), "--load-extension=" + str(stage),
                      "--remote-debugging-port=0"])
            try:
                sw = ctx.service_workers[0] if ctx.service_workers else ctx.wait_for_event("serviceworker", timeout=30000)
                check("extension loads with the expected id", EXT_ID in sw.url, sw.url)
                ctx.set_default_timeout(20000)
                page = ctx.new_page()
                page.goto(base)
                print("fixture page loaded", flush=True)
                tab_id = sw.evaluate("Promise.race([new Promise(r => chrome.tabs.query({}, t => "
                                     "r(t.map(x => [x.id, x.url])))), "
                                     "new Promise((_, j) => setTimeout(() => j('tabs.query timeout'), 10000))])")
                print("tabs:", tab_id, flush=True)
                tab_id = next(i for i, u in tab_id if (u or "").startswith("http://127.0.0.1"))
                popup_url = "chrome-extension://%s/popup.html" % EXT_ID

                def select_para():
                    page.evaluate("document.activeElement.blur(); const r=document.createRange(); "
                                  "r.selectNodeContents(document.getElementById('para')); "
                                  "const s=getSelection(); s.removeAllRanges(); s.addRange(r)")

                def clear_sel():
                    page.evaluate("document.activeElement.blur(); getSelection().removeAllRanges()")

                def open_popup(url=None, title="Fixture page", extra_init=""):
                    pp = ctx.new_page()
                    pp.add_init_script("(() => { const t = {id: %d, url: %s, title: %s, active: true};"
                                       " chrome.tabs.query = (q, cb) => cb([t]); })();"
                                       % (tab_id, json.dumps(url or base), json.dumps(title)))
                    if extra_init:
                        pp.add_init_script(extra_init)
                    pp.goto(popup_url)
                    return pp

                def form_ready(pp, timeout=20000):
                    try:
                        pp.wait_for_selector("#form:not([hidden])", timeout=timeout)
                    except Exception:
                        print("DIAG popup never showed its form; body text: %r" % pp.locator("body").inner_text()[:300],
                              flush=True)
                        raise

                def status(pp):
                    return pp.locator("#status").inner_text() if pp.locator("#status").is_visible() else ""

                # --- popup: normal capture
                select_para()
                pp = open_popup()
                form_ready(pp)
                check("popup shows selection", "Buy oranges" in pp.locator("#preview").inner_text())
                check("popup shows source page and url",
                      pp.locator("#page").inner_text() == "Fixture page" and pp.locator("#url").inner_text() == base)
                check("popup derives title", pp.locator("#title").input_value().startswith("Buy oranges"))
                check("capture button enabled", pp.locator("#capture").is_enabled())
                pp.locator("#capture").click()
                wait_for(lambda: "Captured in TaskNest." in status(pp), 15)
                check("popup reports captured", "Captured in TaskNest." in status(pp), status(pp))
                rows = db_rows(db)
                check("exactly one task stored", len(rows) == 1, rows)
                if rows:
                    check("task notes end with Source line", rows[0][2].endswith("Source: " + base), rows[0][2])
                    check("html in selection stored literally", "<b>literal</b>" in rows[0][2], rows[0][2])
                check("button disabled after capture", pp.locator("#capture").is_disabled())
                pp.locator("#title").press("Enter")
                time.sleep(1)
                check("Enter after capture creates no second task", len(db_rows(db)) == 1)
                pp.close()

                # --- popup: Enter in an edited title
                pp = open_popup()
                form_ready(pp)
                pp.locator("#title").fill("Edited e2e title")
                pp.locator("#title").press("Enter")
                wait_for(lambda: len(db_rows(db)) == 2, 15)
                rows = db_rows(db)
                check("Enter in title captures with edited title", len(rows) == 2 and rows[-1][1] == "Edited e2e title", rows)
                pp.close()

                # --- the real action popup (chrome.action.openPopup), real chrome.tabs.query, no stub.
                # openPopup does not grant activeTab (only a user click does), so this relies on the
                # staged host_permissions for 127.0.0.1. It proves the real popup page, its real
                # active-tab lookup, selection read and capture; it does not prove the toolbar click.
                n0 = len(db_rows(db))
                select_para()
                page.bring_to_front()
                opened = sw.evaluate("chrome.action.openPopup().then(() => 'opened', e => 'rejected: ' + e.message)")
                check("chrome.action.openPopup opens the real popup", opened == "opened", opened)
                port_file = scratch / "browser-data" / "DevToolsActivePort"
                wait_for(port_file.exists, 10)
                port = port_file.read_text().split()[0] if port_file.exists() else None
                br = pw.chromium.connect_over_cdp("http://127.0.0.1:%s" % port)
                try:
                    rp = None
                    def find_popup():
                        nonlocal rp
                        for c in br.contexts:
                            for pg in c.pages:
                                if pg.url.endswith("/popup.html"):
                                    rp = pg
                        return rp is not None
                    wait_for(find_popup, 10)
                    check("real popup page found", rp is not None)
                    if rp:
                        form_ready(rp)
                        check("real popup reads the page selection and url",
                              "Buy oranges" in rp.locator("#preview").inner_text()
                              and rp.locator("#url").inner_text() == base, rp.locator("#url").inner_text())
                        t_click = time.perf_counter()
                        rp.locator("#capture").click()
                        wait_for(lambda: status(rp) != "", 15)
                        wait_for(lambda: "Captured" in status(rp) or UNKNOWN_TEXT in status(rp), 15)
                        latencies.append((time.perf_counter() - t_click) * 1000)
                        retries = 0
                        while UNKNOWN_TEXT in status(rp) and retries < 3:
                            # The popup could not confirm the capture. That is an unknown outcome, not a
                            # failure: the task may exist. Same popup => same request id, so pressing the
                            # button again must never create a second task. Reported, not hidden.
                            retries += 1
                            unknown_outcomes.append("real popup capture")
                            stored_now = len(db_rows(db)) - n0
                            print("NOTE real popup: outcome unknown (retry %d, same request id); "
                                  "tasks stored so far: %d" % (retries, stored_now), flush=True)
                            check("unknown outcome: popup does not claim failure",
                                  "failed" not in status(rp).lower() and "Check again" in rp.locator("#capture").inner_text(),
                                  status(rp))
                            check("unknown outcome: at most one task stored before the retry (retry %d)" % retries,
                                  stored_now in (0, 1), stored_now)
                            time.sleep(1)
                            rp.locator("#capture").click()
                            wait_for(lambda: "Captured" in status(rp) or UNKNOWN_TEXT in status(rp), 15)
                        okc = "Captured in TaskNest." in status(rp) and len(db_rows(db)) == n0 + 1
                        if retries:
                            check("same-id retry after unknown left exactly one task",
                                  len(db_rows(db)) == n0 + 1, len(db_rows(db)))
                        if not okc:  # diagnostics for a failed capture: late write? app still answering?
                            t0 = time.time()
                            time.sleep(8)
                            print("DIAG status=%r rows %d->%d after 8s; host_status=%s (%.1fs)"
                                  % (status(rp), n0, len(db_rows(db)), host_status(bat), time.time() - t0), flush=True)
                        check("real popup captures into the scratch database", okc, status(rp))
                    if rp:
                        rp.close()
                finally:
                    br.close()

                # --- popup: unknown outcome handling (real popup page, stubbed native messaging so the
                # outcome is deterministic; the stub never reaches the host, so no task may appear)
                STUB = """(() => {
                    window.__calls = []; window.__mode = 'exit';
                    chrome.runtime.sendNativeMessage = (host, msg) => {
                        window.__calls.push(msg);
                        return new Promise((res, rej) => setTimeout(() => {
                            if (window.__mode === 'exit') rej(new Error('Native host has exited.'));
                            else if (window.__mode === 'missing') rej(new Error('Specified native messaging host not found.'));
                            else res({ok: true, result: {task_id: 4242, duplicate: true}});
                        }, 600));
                    };
                })();"""
                n_stub = len(db_rows(db))
                select_para()
                sp = open_popup(extra_init=STUB)
                form_ready(sp)
                for _ in range(4):  # repeated clicks / Enter while the first attempt is pending
                    sp.locator("#capture").click(force=True)
                sp.locator("#title").press("Enter")
                wait_for(lambda: UNKNOWN_TEXT in status(sp), 10)
                calls = sp.evaluate("window.__calls")
                check("repeated clicks while pending send exactly one request", len(calls) == 1, len(calls))
                check("unknown outcome: message says it may or may not be saved", UNKNOWN_TEXT in status(sp), status(sp))
                check("unknown outcome: popup does not call it a failure",
                      "failed" not in status(sp).lower() and sp.locator("#status").get_attribute("class") == "warn")
                check("unknown outcome: button stays actionable as 'Check again'",
                      sp.locator("#capture").is_enabled() and sp.locator("#capture").inner_text() == "Check again")
                check("unknown outcome: title locked so the retry matches the original",
                      sp.locator("#title").is_disabled())
                sp.evaluate("window.__mode = 'ok'")
                sp.locator("#capture").click()
                wait_for(lambda: "Already captured" in status(sp) or "Captured" in status(sp), 10)
                calls = sp.evaluate("window.__calls")
                check("retry reuses the original request id",
                      len(calls) == 2 and calls[0].get("request_id") and calls[0]["request_id"] == calls[1]["request_id"],
                      [c.get("request_id") for c in calls])
                check("retry result is shown as the original capture", "Already captured" in status(sp), status(sp))
                check("stubbed unknown/retry cycle created no task", len(db_rows(db)) == n_stub)
                first_id = calls[0]["request_id"] if calls else None
                sp.close()
                sp = open_popup(extra_init=STUB)
                form_ready(sp)
                sp.evaluate("window.__mode = 'ok'")
                sp.locator("#capture").click()
                wait_for(lambda: "Captured" in status(sp), 10)
                calls = sp.evaluate("window.__calls")
                check("a new popup (new capture of the same page) uses a new request id",
                      len(calls) == 1 and calls[0]["request_id"] != first_id)
                sp.close()
                sp = open_popup(extra_init=STUB)
                form_ready(sp)
                sp.evaluate("window.__mode = 'missing'")
                sp.locator("#capture").click()
                wait_for(lambda: "not installed" in status(sp), 10)
                check("host not installed is a confirmed failure, not an unknown outcome",
                      "not installed" in status(sp) and UNKNOWN_TEXT not in status(sp)
                      and sp.locator("#capture").inner_text() != "Check again", status(sp))
                sp.close()

                # --- popup: refusals create no task
                n = len(db_rows(db))
                clear_sel()
                pp = open_popup()
                form_ready(pp)
                check("no selection: guidance shown", "Select some text" in status(pp), status(pp))
                check("no selection: capture disabled", pp.locator("#capture").is_disabled())
                pp.close()

                page.evaluate("const i=document.getElementById('inp'); i.focus(); i.select()")
                pp = open_popup()
                wait_for(lambda: "form field" in status(pp), 10)
                check("selection in input field is refused", "form field" in status(pp), status(pp))
                pp.close()

                select_para()
                pp = open_popup(url="file:///C:/x.txt")
                wait_for(lambda: status(pp) != "", 10)
                check("non-http page is refused", "Only http and https" in status(pp), status(pp))
                pp.close()
                check("refusals created no task", len(db_rows(db)) == n)

                # --- popup: TaskNest unavailable states
                def failing(expect, name):
                    select_para()
                    p2 = open_popup()
                    form_ready(p2)
                    p2.locator("#capture").click()
                    wait_for(lambda: expect in status(p2), 20)
                    check(name, expect in status(p2), status(p2))
                    p2.close()

                app.quit()
                set_integration(db, "0")
                app.start()
                wait_for(lambda: app.running() and (profile / "data" / "tasknest.db.lock").exists(), 30)
                time.sleep(1.5)
                st = host_status(bat)
                check("host reports integration_disabled for scratch profile",
                      st.get("ok") is False and st.get("error", {}).get("code") == "integration_disabled", st)
                failing("integration API is off", "API off: clear message")
                check("API off: no task created", len(db_rows(db)) == n)
                app.quit()
                st = host_status(bat)
                check("host reports not_running for scratch profile",
                      st.get("ok") is False and st.get("error", {}).get("code") == "not_running", st)
                failing("TaskNest is not running", "TaskNest closed: clear message")
                check("TaskNest closed: no task created", len(db_rows(db)) == n)

                # --- context-menu logic: the real background.js against a mocked chrome object
                core = (EXT_SRC / "capture-core.js").read_text(encoding="utf-8")
                bg = (EXT_SRC / "background.js").read_text(encoding="utf-8")
                o = page.evaluate(BACKGROUND_PROBE, [core, bg])
                c = o["calls"]
                check("context menu item registered for selections",
                      c["removed"] == 1 and len(c["created"]) == 1 and c["created"][0]["id"] == "tasknest-capture-selection"
                      and c["created"][0]["contexts"] == ["selection"], c["created"])
                check("context menu ignores other menu ids", o["afterOther"] == 0)
                check("context menu refuses empty selection with ! badge",
                      o["afterEmpty"] == 0 and o["badgesAfterEmpty"] == ["!"], o["badgesAfterEmpty"])
                check("context menu allows one capture per tab at a time",
                      o["afterDouble"] == 1 and c["sent"][0][1]["op"] == "capture", o["afterDouble"])
                check("context menu success badge", "✓" in o["badgesAfterOk"], o["badgesAfterOk"])
                check("context menu failure badge", o["badgesFinal"][-1] == "!", o["badgesFinal"])
            finally:
                ctx.close()
    finally:
        problems = []
        if server:
            server.shutdown()
            server.server_close()
        if not app.quit():
            problems.append("scratch TaskNest had to be killed")
        if registered:
            try:
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, CHROMIUM_NM + "\\" + HOST_NAME)
                if parent_created:  # this run created the parent too; remove it only if empty
                    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, CHROMIUM_NM)
            except OSError as e:
                problems.append("registry cleanup failed: %r" % e)
        # anything still carrying the unique scratch path is harness-owned (browser, crashpad, host)
        leftovers = kill_by_path_fragment(str(scratch))
        check("no leftover browser/host processes", not leftovers, leftovers)
        check("scratch TaskNest process gone", not app.running())
        ok, remaining = remove_scratch(scratch, log=lambda m: print(m, flush=True))
        check("scratch folder removed", ok, remaining)
        if not ok:
            print("CLEANUP FAILED, remaining:", flush=True)
            for p, why in remaining:
                print("  %s  (%s)" % (p, why), flush=True)
        for pr in problems:
            check("cleanup: " + pr, False)

    check("live database untouched", sha(LIVE_DB) == before_db)
    check("registry identical to before the run", reg_snapshot() == before_reg)
    failed = [n for n, ok in results if not ok]
    # machine-readable, for comparing runs: click -> confirmed result in the real popup
    print("CAPTURE_LATENCY_MS %s UNKNOWN_OUTCOMES %d" % (
        json.dumps([round(x) for x in latencies]), len(unknown_outcomes)), flush=True)
    print("\nSUMMARY: %d passed, %d failed" % (len(results) - len(failed), len(failed)))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
