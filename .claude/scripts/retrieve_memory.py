"""Search previous work. Usage: python retrieve_memory.py "query" [-n 5] [--all] [--kind procedure]"""
import argparse, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m


def retrieve(query, n=5, include_superseded=False, kinds=None):
    con = m.connect(create=False)
    if con is None:
        return []
    return m.search(con, query, n, include_superseded, kinds)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("query")
    ap.add_argument("-n", type=int, default=5)
    ap.add_argument("--all", action="store_true", help="include superseded (outdated) records")
    ap.add_argument("--kind", action="append")
    a = ap.parse_args()
    hits = [h for h in retrieve(a.query, a.n, a.all, a.kind) if h["score"] >= 0.15]
    if not hits:
        print("No matching memory.")
        return
    for h in hits:
        print(f"[score {h['score']}]", m.fmt(h))
    m.log_event("retrieve", n=len(hits))


if __name__ == "__main__":
    main()
