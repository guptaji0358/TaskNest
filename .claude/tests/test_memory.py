"""Tests for the local memory system. Run: python -m unittest discover -s .claude/tests -v
Uses a temporary DB/log via env vars; never touches the real memory.db."""
import gc, json, os, socket, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "scripts"
TMP = tempfile.mkdtemp(prefix="tnmem-test-")
os.environ["TNMEM_DB"] = str(Path(TMP) / "memory.db")
os.environ["TNMEM_LOG"] = str(Path(TMP) / "log.jsonl")
sys.path.insert(0, str(SCRIPTS))
import tnmem as m
import detect_repetition as d
import verify_memory as v


_open = []
_orig_connect = m.connect


def _tracking_connect(*a, **k):
    c = _orig_connect(*a, **k)
    if c is not None:
        _open.append(c)
    return c


m.connect = _tracking_connect  # modules call m.connect via the module attribute, so this covers them


def fresh():
    while _open:
        try:
            _open.pop().close()
        except Exception:
            pass
    gc.collect()
    for p in Path(TMP).glob("memory.db*"):
        p.unlink()


def run(script, *args, stdin=None, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([sys.executable, str(script), *args], input=stdin, capture_output=True, text=True, env=e)


class T(unittest.TestCase):
    def setUp(self):
        fresh()

    def seed(self):
        con = m.connect()
        self.a = m.add_record(con, "task", "Verify all OpenAPI references and ServerBusy operation scope",
                              "Ran python resolve script; 503 ServerBusy limited to sync ops.", "completed", 0.9,
                              "api/openapi.yaml", keywords="yaml $ref 503")
        self.b = m.add_record(con, "procedure", "Build and run CTest for TaskNest",
                              "cmake --build out/build; ctest --test-dir out/build", "verified", 0.9, "CLAUDE.md")
        return con

    def test_duplicate(self):
        self.seed()
        r = d.detect("Check whether the OpenAPI YAML references resolve and the 503 scope is unchanged")
        self.assertEqual(r["verdict"], "duplicate")
        self.assertEqual(r["hits"][0]["r"]["id"], self.a)

    def test_related_not_duplicate(self):
        self.seed()
        r = d.detect("Add a new OpenAPI endpoint for device revocation")
        self.assertIn(r["verdict"], ("related", "new"))
        self.assertNotEqual(r["verdict"], "duplicate")

    def test_unrelated_is_new(self):
        self.seed()
        self.assertEqual(d.detect("Design the Android notification channel layout")["verdict"], "new")

    def test_retrieval_relevance(self):
        self.seed()
        hits = m.search(m.connect(), "how do I run the ctest suite")
        self.assertEqual(hits[0]["r"]["id"], self.b)

    def test_supersession_hides_outdated(self):
        con = self.seed()
        new = m.add_record(con, "procedure", "Build and run CTest for TaskNest with Debug",
                           "cmake --build out/build --config Debug", "verified", 0.9, "x", supersedes=self.b)
        ids = [h["r"]["id"] for h in m.search(con, "build and run ctest")]
        self.assertIn(new, ids)
        self.assertNotIn(self.b, ids)
        old = [h for h in m.search(con, "build and run ctest", include_superseded=True) if h["r"]["id"] == self.b][0]
        self.assertIn("OUTDATED", m.fmt(old))
        self.assertEqual(old["r"]["superseded_by"], new)

    def test_update_by_key_no_duplicate(self):
        con = m.connect()
        a = m.add_record(con, "fact", "Qt version fact", "6.12", "verified", 0.8, "s", key="k1")
        b = m.add_record(con, "fact", "Qt version fact", "6.12.1", "verified", 0.8, "s", key="k1")
        self.assertEqual(a, b)
        self.assertEqual(con.execute("SELECT count(*) FROM records").fetchone()[0], 1)

    def test_failed_and_assumption_labelled(self):
        con = m.connect()
        m.add_record(con, "task", "Try sqlite vacuum on live db", "corrupted nothing but slow", "failed", 0.5, "x")
        h = m.search(con, "sqlite vacuum live db")[0]
        self.assertIn("FAILED ATTEMPT", m.fmt(h))

    def test_empty_memory(self):
        self.assertEqual(d.detect("verify the openapi references resolve")["verdict"], "new")
        self.assertEqual(m.search(m.connect(), "anything at all here"), [])
        p, _ = v.check()
        self.assertEqual(p, [])

    def test_missing_db_not_created_by_read(self):
        fresh()
        self.assertIsNone(m.connect(create=False))
        r = run(SCRIPTS / "retrieve_memory.py", "some query words")
        self.assertEqual(r.returncode, 0)

    def test_corrupt_db_recovers(self):
        Path(os.environ["TNMEM_DB"]).write_bytes(b"this is not a sqlite database" * 50)
        con = m.connect()
        self.assertEqual(con.execute("SELECT count(*) FROM records").fetchone()[0], 0)
        self.assertTrue(list(Path(TMP).glob("memory.db.corrupt-*")))  # original preserved, not deleted

    def test_verify_detects_corruption(self):
        Path(os.environ["TNMEM_DB"]).write_bytes(b"garbage" * 100)
        p, _ = v.check()
        self.assertTrue(p)

    def test_secret_exclusion(self):
        con = m.connect()
        rid = m.add_record(con, "fact", "Config note", "api_key=sk-abcdefghijklmnopqrstuvwx and token: abc123def "
                           "postgresql://u:hunter2@host/db", "verified", 0.5, "x")
        row = con.execute("SELECT body FROM records WHERE id=?", (rid,)).fetchone()[0]
        for s in ("sk-abcdef", "abc123def", "hunter2"):
            self.assertNotIn(s, row)
        self.assertIn("[REDACTED]", row)
        self.assertEqual(m.search(con, "hunter2 config note")[0]["r"]["id"], rid)  # found by title only, not secret
        self.assertNotIn("hunter2", json.dumps([h["r"] for h in m.search(con, "hunter2 config note")]))

    def test_save_cli_redacts_and_mirrors(self):
        r = run(SCRIPTS / "save_task.py", "--title", "Deploy note", "--body", "password=SuperSecret99", "--source", "t",
                env={"TNMEM_CLAUDE_DIR": TMP})
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("redacted", r.stdout)
        for f in Path(TMP, "memory").rglob("*.json"):
            self.assertNotIn("SuperSecret99", f.read_text())

    def test_index_md_and_stale_cleanup(self):
        mem = Path(TMP, "memory"); mem.mkdir(exist_ok=True)
        (mem / "a.md").write_text("# A\n## Build\nUse cmake and ninja to build\n## Old\nobsolete section text\n")
        env = {"TNMEM_CLAUDE_DIR": TMP}
        self.assertEqual(run(SCRIPTS / "index_memory.py", env=env).returncode, 0)
        con = m.connect()
        self.assertTrue(m.search(con, "cmake ninja build"))
        (mem / "a.md").write_text("# A\n## Build\nUse cmake and ninja to build\n")
        run(SCRIPTS / "index_memory.py", env=env)
        self.assertFalse(m.search(m.connect(), "obsolete section text"))
        (mem / "a.md").unlink()

    def test_no_network(self):
        real = socket.socket
        def boom(*a, **k):
            raise AssertionError("network used")
        socket.socket = boom
        try:
            self.seed()
            d.detect("verify openapi references resolve scope")
        finally:
            socket.socket = real
        src = " ".join(p.read_text() for p in SCRIPTS.glob("*.py")) + (ROOT / "hooks/memory_hook.py").read_text()
        for bad in ("import requests", "urllib.request", "http.client", "import socket", "urlopen"):
            self.assertNotIn(bad, src)

    def test_hook_outputs_context(self):
        self.seed()
        r = run(ROOT / "hooks/memory_hook.py", "prompt",
                stdin=json.dumps({"prompt": "Check whether the OpenAPI YAML references resolve and the 503 scope is unchanged"}))
        self.assertEqual(r.returncode, 0)
        out = json.loads(r.stdout)["hookSpecificOutput"]
        self.assertEqual(out["hookEventName"], "UserPromptSubmit")
        self.assertIn("ServerBusy", out["additionalContext"])

    def test_hook_silent_when_nothing_relevant(self):
        self.seed()
        r = run(ROOT / "hooks/memory_hook.py", "prompt", stdin=json.dumps({"prompt": "hello there friend, how are you"}))
        self.assertEqual((r.returncode, r.stdout.strip()), (0, ""))

    def test_hook_failure_never_blocks(self):
        for stdin in ("not json at all", "", "[1,2]", json.dumps({"prompt": None})):
            r = run(ROOT / "hooks/memory_hook.py", "prompt", stdin=stdin)
            self.assertEqual(r.returncode, 0, stdin)
        Path(os.environ["TNMEM_DB"]).write_bytes(b"corrupt" * 100)
        r = run(ROOT / "hooks/memory_hook.py", "prompt", stdin=json.dumps({"prompt": "verify openapi references resolve scope"}))
        self.assertEqual(r.returncode, 0)
        r = run(ROOT / "hooks/memory_hook.py", "session", stdin="{}")
        self.assertEqual(r.returncode, 0)

    def test_auto_capture_on_stop(self):
        h = ROOT / "hooks/memory_hook.py"
        p = "Investigate why the reminder scheduler skips overdue tasks after resume"
        run(h, "prompt", stdin=json.dumps({"prompt": p, "session_id": "s1"}))
        ans = "Root cause: timer re-armed against stale due time. " * 12
        for _ in range(2):  # second identical task updates, not duplicates
            run(h, "prompt", stdin=json.dumps({"prompt": p, "session_id": "s1"}))
            r = run(h, "stop", stdin=json.dumps({"session_id": "s1", "last_assistant_message": ans + " token=abc123xyz"}))
            self.assertEqual(r.returncode, 0)
        con = m.connect()
        rows = con.execute("SELECT * FROM records WHERE key LIKE 'auto:%'").fetchall()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["status"], "assumption")
        self.assertNotIn("abc123xyz", rows[0]["body"])
        # short/trivial turns and stop_hook_active are not captured
        run(h, "prompt", stdin=json.dumps({"prompt": "what time zone is used for storage here", "session_id": "s2"}))
        run(h, "stop", stdin=json.dumps({"session_id": "s2", "last_assistant_message": "UTC."}))
        run(h, "stop", stdin=json.dumps({"session_id": "s1", "stop_hook_active": True, "last_assistant_message": ans}))
        self.assertEqual(con.execute("SELECT count(*) FROM records WHERE key LIKE 'auto:%'").fetchone()[0], 1)

    def test_settings_json_valid(self):
        s = json.loads((ROOT / "settings.json").read_text())
        for ev in ("SessionStart", "UserPromptSubmit"):
            h = s["hooks"][ev][0]["hooks"][0]
            self.assertEqual(h["type"], "command")
            self.assertIn("memory_hook.py", h["command"])


if __name__ == "__main__":
    unittest.main()
