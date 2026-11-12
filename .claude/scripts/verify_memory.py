"""Integrity check. Exit 0 = ok, 1 = problems. Usage: python verify_memory.py"""
import sqlite3, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m


def check():
    probs = []
    if not m.DB_PATH.exists():
        return ["memory.db missing (run index_memory.py)"], {}
    con = sqlite3.connect(str(m.DB_PATH))
    con.row_factory = sqlite3.Row
    try:
        if con.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
            probs.append("SQLite integrity_check failed")
        rows = con.execute("SELECT * FROM records").fetchall()
        fts = {r[0] for r in con.execute("SELECT rowid FROM records_fts")}
    except sqlite3.DatabaseError as e:
        return [f"database unreadable: {e}"], {}
    finally:
        con.close()
    ids = {r["id"] for r in rows}
    if ids - fts:
        probs.append(f"{len(ids - fts)} record(s) missing from FTS index (run index_memory.py --rebuild)")
    if fts - ids:
        probs.append(f"{len(fts - ids)} orphan FTS row(s)")
    for r in rows:
        if r["status"] not in m.STATUSES or r["kind"] not in m.KINDS:
            probs.append(f"#{r['id']}: bad kind/status")
        if r["status"] == "superseded" and r["superseded_by"] not in ids:
            probs.append(f"#{r['id']}: superseded_by points to missing record")
        if r["status"] in ("verified", "approved", "completed") and not r["source"]:
            probs.append(f"#{r['id']}: {r['status']} without source")
        for f in (r["title"], r["body"], r["keywords"]):
            if m.redact(f)[1]:
                probs.append(f"#{r['id']}: secret-like text present")
                break
    by = {}
    for r in rows:
        by[r["status"]] = by.get(r["status"], 0) + 1
    return probs, dict(total=len(rows), by_status=by)


if __name__ == "__main__":
    probs, stats = check()
    print("stats:", stats)
    for p in probs:
        print("PROBLEM:", p)
    print("OK" if not probs else f"{len(probs)} problem(s)")
    sys.exit(1 if probs else 0)
