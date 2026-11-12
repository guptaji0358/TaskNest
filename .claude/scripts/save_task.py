"""Save a verified task outcome / decision / procedure. Secrets are redacted before storage.
Usage: python save_task.py --title "..." --body "..." [--kind task|decision|procedure|summary|fact]
         [--status verified|approved|assumption|failed|completed|pending] [--confidence 0.8]
         [--source "file or command"] [--keywords "..."] [--supersedes ID]
Also writes a JSON mirror into memory/<kind dir>/ so records survive a lost memory.db (re-run index_memory.py)."""
import argparse, json, re, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m

DIRS = {"task": "task_records", "decision": "decisions", "procedure": "procedures", "summary": "summaries", "fact": "summaries"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", required=True)
    ap.add_argument("--body", default="")
    ap.add_argument("--kind", default="task", choices=m.KINDS)
    ap.add_argument("--status", default="verified", choices=m.STATUSES)
    ap.add_argument("--confidence", type=float, default=0.8)
    ap.add_argument("--source", default="")
    ap.add_argument("--keywords", default="")
    ap.add_argument("--supersedes", type=int)
    a = ap.parse_args()
    if not a.source and a.status in ("verified", "approved", "completed"):
        sys.stderr.write("warning: verified/approved record without --source; add a file/command reference\n")
    title, nt = m.redact(a.title)
    body, nb = m.redact(a.body)
    kw, nk = m.redact(a.keywords)
    con = m.connect()
    rid = m.add_record(con, a.kind, title, body, a.status, a.confidence, a.source, kw, supersedes=a.supersedes)
    slug = re.sub(r"[^a-z0-9]+", "-", title.lower())[:50].strip("-") or "record"
    d = m.MEM_DIR / DIRS[a.kind]
    d.mkdir(parents=True, exist_ok=True)
    rec = dict(kind=a.kind, title=title, body=body, status=a.status, confidence=a.confidence,
               source=a.source, keywords=kw, saved=m.now(), supersedes=a.supersedes)
    fname = f"{rid:04d}-{slug}.json"
    (d / fname).write_text(json.dumps(rec, indent=2), encoding="utf-8")
    # same key index_memory.py uses, so re-indexing updates this row instead of duplicating it
    con.execute("UPDATE records SET key=? WHERE id=?", (f"json:{DIRS[a.kind]}/{fname}", rid))
    con.commit()
    note = f" ({nt + nb + nk} secret-like value(s) redacted)" if nt + nb + nk else ""
    print(f"saved #{rid} [{a.kind}/{a.status}]{note}" + (f"; superseded #{a.supersedes}" if a.supersedes else ""))
    m.log_event("save", id=rid, kind=a.kind)


if __name__ == "__main__":
    main()
