"""Classify a task as duplicate / related / new against memory.
Usage: python detect_repetition.py "task description" [--json]"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m

TASK_KINDS = ("task", "procedure", "decision")


def detect(text, limit=3):
    """Returns dict(verdict, fingerprint, hits). Only task/procedure/decision records count as repeat evidence;
    doc summaries are returned as context but never make a task a 'duplicate'."""
    con = m.connect(create=False)
    res = dict(verdict="new", fingerprint=m.fingerprint(text), hits=[])
    if con is None:
        return res
    hits = [h for h in m.search(con, text, limit * 3) if h["score"] >= 0.15]
    evidence = [h for h in hits if h["r"]["kind"] in TASK_KINDS and h["r"]["status"] != "pending"]
    best = evidence[0] if evidence else None
    res["verdict"] = m.classify(best)
    # exact fingerprint match against a completed/verified task is always a duplicate
    if con.execute("SELECT 1 FROM records WHERE fingerprint=? AND kind='task' AND status IN ('completed','verified')",
                   (res["fingerprint"],)).fetchone():
        res["verdict"] = "duplicate"
    res["hits"] = hits[:limit]
    return res


def main():
    args = [a for a in sys.argv[1:] if a != "--json"]
    if not args:
        sys.exit("usage: detect_repetition.py 'task' [--json]")
    r = detect(" ".join(args))
    if "--json" in sys.argv:
        print(json.dumps(dict(r, hits=[dict(score=h["score"], **h["r"]) for h in r["hits"]]), indent=2))
        return
    print(f"verdict: {r['verdict'].upper()}  fingerprint: {r['fingerprint']}")
    for h in r["hits"]:
        print(m.fmt(h))


if __name__ == "__main__":
    main()
