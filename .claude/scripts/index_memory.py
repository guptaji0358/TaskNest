"""Index .claude/memory/*.md (by ## section) and task_records|decisions|procedures/*.json into memory.db.
Usage: python index_memory.py [--rebuild]"""
import json, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m

JSON_DIRS = {"task_records": "task", "decisions": "decision", "procedures": "procedure", "summaries": "summary"}


def md_sections(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    title, buf, out = path.stem, [], []
    for line in text.splitlines():
        if line.startswith("#"):
            if buf and "".join(buf).strip():
                out.append((title, "\n".join(buf)))
            title, buf = line.lstrip("# ").strip() or path.stem, []
        else:
            buf.append(line)
    if buf and "".join(buf).strip():
        out.append((title, "\n".join(buf)))
    return out


def main(argv):
    con = m.connect()
    if "--rebuild" in argv:
        con.execute("DELETE FROM records WHERE key LIKE 'md:%' OR key LIKE 'json:%'")
        con.execute("DELETE FROM records_fts WHERE rowid NOT IN (SELECT id FROM records)")
    seen, n = set(), 0
    for p in sorted(m.MEM_DIR.glob("*.md")):
        for title, body in md_sections(p):
            key = f"md:{p.name}#{title}"
            seen.add(key)
            # project memory files are navigation aids, not authoritative: moderate confidence
            m.add_record(con, "summary", f"{p.stem}: {title}", body, "verified", 0.6,
                         f".claude/memory/{p.name}", key=key)
            n += 1
    for d, kind in JSON_DIRS.items():
        for p in sorted((m.MEM_DIR / d).glob("*.json")):
            try:
                o = json.loads(p.read_text(encoding="utf-8"))
                key = f"json:{d}/{p.name}"
                seen.add(key)
                m.add_record(con, o.get("kind", kind), o["title"], o.get("body", ""), o.get("status", "verified"),
                             float(o.get("confidence", 0.7)), o.get("source", f"{d}/{p.name}"),
                             o.get("keywords", ""), key=key)
                n += 1
            except Exception as e:
                sys.stderr.write(f"skip {p.name}: {e}\n")
    # drop index entries whose source section/file vanished (never touches manually saved records)
    for r in con.execute("SELECT id,key FROM records WHERE key LIKE 'md:%' OR key LIKE 'json:%'").fetchall():
        if r["key"] not in seen:
            con.execute("DELETE FROM records WHERE id=?", (r["id"],))
            con.execute("DELETE FROM records_fts WHERE rowid=?", (r["id"],))
    con.commit()
    print(f"indexed {n} records from files; total in db: {con.execute('SELECT count(*) FROM records').fetchone()[0]}")


if __name__ == "__main__":
    main(sys.argv[1:])
