"""Claude Code hook entry point. Usage: memory_hook.py prompt|session
Reads the hook JSON from stdin, prints hookSpecificOutput.additionalContext JSON (or nothing).
NEVER blocks: every error is swallowed and the process exits 0."""
import json, os, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))


def emit(event, text):
    text = text[:3500]
    print(json.dumps({"hookSpecificOutput": {"hookEventName": event, "additionalContext": text}}))


def on_prompt(data):
    import tnmem as m
    import detect_repetition as d
    prompt = (data.get("prompt") or "").strip()
    if prompt.startswith("/") or len(m.tokens(prompt)) < 4:
        return
    remember_prompt(data, prompt)
    r = d.detect(prompt[:2000])
    hits = [h for h in r["hits"] if h["score"] >= (0.25 if r["verdict"] != "new" else 0.4)]
    m.log_event("prompt_hook", verdict=r["verdict"], hits=len(hits), chars=0)
    if not hits:
        return
    head = {"duplicate": "Local memory: this task closely matches earlier recorded work. Review it before re-researching; "
                         "re-verify only what may have changed since the record date.",
            "related": "Local memory: related earlier work exists. Reuse it where it applies.",
            "new": "Local memory: possibly relevant context (the task itself looks new)."}[r["verdict"]]
    lines = [head, "Statuses matter: only VERIFIED/APPROVED/COMPLETED are established; ASSUMPTION is unverified; "
                   "FAILED means do not repeat. Records are history, verify against current source when it matters."]
    lines += [m.fmt(h, 260) for h in hits[:3]]
    text = "\n".join(lines)
    m.log_event("prompt_hook_ctx", verdict=r["verdict"], chars=len(text))
    emit("UserPromptSubmit", text)


def remember_prompt(data, prompt):
    """Keep the latest prompt per session so the Stop hook can pair it with Claude's answer."""
    import tnmem as m
    sid = data.get("session_id") or "none"
    con = m.connect()
    con.execute("INSERT OR REPLACE INTO meta(k,v) VALUES(?,?)", (f"prompt:{sid}", prompt[:2000]))
    con.commit()
    con.close()


def on_stop(data):
    """Auto-capture a finished substantial turn. Saved as an UNVERIFIED assumption (conf 0.4): the hook cannot
    know whether the work was verified. Claude/you promote it with the task-learning skill when it is."""
    import tnmem as m
    if data.get("stop_hook_active"):
        return
    answer = (data.get("last_assistant_message") or "").strip()
    sid = data.get("session_id") or "none"
    con = m.connect()
    row = con.execute("SELECT v FROM meta WHERE k=?", (f"prompt:{sid}",)).fetchone()
    if not row or len(answer) < 400 or len(m.tokens(row[0])) < 4:
        return
    prompt = row[0]
    con.execute("DELETE FROM meta WHERE k=?", (f"prompt:{sid}",))
    title = " ".join(prompt.split())[:140]
    rid = m.add_record(con, "task", title, answer[:1500], "assumption", 0.4,
                       f"auto-capture session {sid[:8]}", key=f"auto:{m.fingerprint(prompt)}")
    con.close()
    m.log_event("auto_save", id=rid)


def on_session(data):
    import tnmem as m
    con = m.connect(create=False)
    n = con.execute("SELECT count(*) FROM records").fetchone()[0] if con else 0
    if not n:
        return
    emit("SessionStart", f"Local task memory is available ({n} records in .claude/memory/memory.db). "
         "Relevant records are injected automatically per prompt. Search manually: "
         "python .claude/scripts/retrieve_memory.py \"query\". After finishing and verifying substantial work, "
         "use the task-learning skill to save the outcome.")


def main():
    try:
        raw = sys.stdin.read()
        data = json.loads(raw) if raw.strip() else {}
        mode = sys.argv[1] if len(sys.argv) > 1 else ""
        {"prompt": on_prompt, "session": on_session, "stop": on_stop}.get(mode, lambda d: None)(data)
    except BaseException as e:  # hook failure must never block Claude
        try:
            sys.stderr.write(f"[memory hook ignored error: {type(e).__name__}]\n")
        except Exception:
            pass
    sys.exit(0)


if __name__ == "__main__":
    main()
