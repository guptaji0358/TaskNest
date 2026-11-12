"""Shared library for the local Claude memory system. Stdlib only, no network."""
import hashlib, json, os, re, sqlite3, sys, time
from pathlib import Path

CLAUDE_DIR = Path(os.environ.get("TNMEM_CLAUDE_DIR") or Path(__file__).resolve().parents[1])
MEM_DIR = CLAUDE_DIR / "memory"
DB_PATH = Path(os.environ.get("TNMEM_DB") or MEM_DIR / "memory.db")
LOG_PATH = Path(os.environ.get("TNMEM_LOG") or CLAUDE_DIR / "reports" / "usage_log.jsonl")

KINDS = ("task", "decision", "procedure", "summary", "fact")
STATUSES = ("verified", "approved", "assumption", "failed", "superseded", "completed", "pending")

STOP = set("""a an the and or of to in on for with from by at as is are was were be been this that these those it its
please can could would should will shall do does did how what why which whether all any some again also just then
about into out up so if not no""".split())
SYN = {"check": "verify", "validate": "verify", "confirm": "verify", "audit": "verify", "ensure": "verify",
       "refs": "reference", "ref": "reference", "bug": "defect", "same": "unchanged"}

SECRET_PATTERNS = [
    re.compile(r"(?i)\b(api[_-]?key|secret|token|passw(?:or)?d|pwd|authorization|bearer|private[_-]?key)\b\s*[:=]\s*\S+"),
    re.compile(r"\bsk-[A-Za-z0-9_\-]{16,}"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(r"\beyJ[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{10,}\.[A-Za-z0-9_\-]{5,}"),
    re.compile(r"(?i)\b(?:postgres(?:ql)?|mysql|mongodb)://[^\s:@]+:[^\s@]+@"),
]


def redact(text):
    """Return (redacted_text, n_redactions)."""
    n = 0
    for p in SECRET_PATTERNS:
        text, k = p.subn("[REDACTED]", text)
        n += k
    return text, n


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def stem(w):
    for suf in ("ing", "ed", "es", "s"):
        if len(w) > len(suf) + 3 and w.endswith(suf):
            return w[: -len(suf)]
    return w


def tokens(text):
    out = []
    for w in re.findall(r"[a-z0-9]+", text.lower()):
        if w in STOP or len(w) < 2:
            continue
        out.append(stem(SYN.get(w, w)))
    return out


def tokset(text):
    return set(tokens(text))


def fingerprint(text):
    return hashlib.sha1(" ".join(sorted(tokset(text))).encode()).hexdigest()[:16]


def dice(a, b):
    if not a or not b:
        return 0.0
    return 2 * len(a & b) / (len(a) + len(b))


SCHEMA = """
CREATE TABLE IF NOT EXISTS records(
  id INTEGER PRIMARY KEY, kind TEXT NOT NULL, title TEXT NOT NULL, body TEXT NOT NULL DEFAULT '',
  keywords TEXT NOT NULL DEFAULT '', status TEXT NOT NULL, confidence REAL NOT NULL DEFAULT 0.7,
  source TEXT NOT NULL DEFAULT '', fingerprint TEXT NOT NULL, created TEXT NOT NULL, updated TEXT NOT NULL,
  superseded_by INTEGER, key TEXT UNIQUE);
CREATE VIRTUAL TABLE IF NOT EXISTS records_fts USING fts5(title, body, keywords);
CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT);
"""


def connect(create=True):
    """Open DB. A corrupt DB is moved aside (never deleted) and a fresh one made."""
    if not DB_PATH.exists() and not create:
        return None
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    for attempt in (0, 1):
        con = None
        try:
            con = sqlite3.connect(str(DB_PATH), timeout=5)
            con.row_factory = sqlite3.Row
            con.executescript(SCHEMA)
            con.execute("SELECT count(*) FROM records").fetchone()
            return con
        except sqlite3.DatabaseError:
            if con is not None:
                con.close()
            if attempt:
                raise
            bak = DB_PATH.with_name(f"memory.db.corrupt-{int(time.time())}")
            DB_PATH.replace(bak)
            sys.stderr.write(f"[memory] corrupt DB moved to {bak.name}; starting empty. Run index_memory.py\n")


def _fts_put(con, rid, title, body, kw):
    con.execute("DELETE FROM records_fts WHERE rowid=?", (rid,))
    con.execute("INSERT INTO records_fts(rowid,title,body,keywords) VALUES(?,?,?,?)", (rid, title, body, kw))


def add_record(con, kind, title, body="", status="verified", confidence=0.7, source="", keywords="",
               key=None, supersedes=None):
    if kind not in KINDS:
        raise ValueError(f"kind must be one of {KINDS}")
    if status not in STATUSES:
        raise ValueError(f"status must be one of {STATUSES}")
    title, _ = redact(title)
    body, _ = redact(body)
    keywords, _ = redact(keywords)
    ts = now()
    fp = fingerprint(title + " " + keywords)
    row = con.execute("SELECT id FROM records WHERE key=?", (key,)).fetchone() if key else None
    if row:
        rid = row["id"]
        con.execute("UPDATE records SET kind=?,title=?,body=?,keywords=?,status=?,confidence=?,source=?,"
                    "fingerprint=?,updated=? WHERE id=?",
                    (kind, title, body, keywords, status, confidence, source, fp, ts, rid))
    else:
        cur = con.execute("INSERT INTO records(kind,title,body,keywords,status,confidence,source,fingerprint,"
                          "created,updated,key) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
                          (kind, title, body, keywords, status, confidence, source, fp, ts, ts, key))
        rid = cur.lastrowid
    _fts_put(con, rid, title, body, keywords)
    if supersedes:
        con.execute("UPDATE records SET status='superseded', superseded_by=?, updated=? WHERE id=? AND id<>?",
                    (rid, ts, supersedes, rid))
    con.commit()
    return rid


def search(con, query, limit=5, include_superseded=False, kinds=None):
    """Hybrid: FTS5 candidates (OR of tokens) re-scored by token Dice overlap."""
    qt = tokset(query)
    if not qt:
        return []
    cands = []
    for r in con.execute("SELECT * FROM records").fetchall():
        cands.append(r)
    out = []
    for r in cands:
        if r["status"] == "superseded" and not include_superseded:
            continue
        if kinds and r["kind"] not in kinds:
            continue
        rt = tokset(r["title"] + " " + r["keywords"])
        full = rt | tokset(r["body"])
        title_sim = dice(qt, rt)
        body_cov = len(qt & full) / len(qt)
        score = max(title_sim, 0.6 * title_sim + 0.32 * body_cov)
        shared = len(qt & rt)
        if qt & full:
            out.append(dict(r=dict(r), score=round(score, 3), shared=shared))
    out.sort(key=lambda x: (-x["score"], -x["r"]["confidence"]))
    return out[:limit]


def classify(hit):
    if hit is None:
        return "new"
    if hit["score"] >= 0.5 and hit["shared"] >= 3:
        return "duplicate"
    if hit["score"] >= 0.25 and hit["shared"] >= 2:
        return "related"
    return "new"


def fmt(hit, width=300):
    r = hit["r"]
    tag = r["status"].upper()
    if r["status"] == "failed":
        tag = "FAILED ATTEMPT - do not repeat"
    elif r["status"] == "superseded":
        tag = f"SUPERSEDED by #{r['superseded_by']} - OUTDATED"
    elif r["status"] == "assumption":
        tag = "UNVERIFIED ASSUMPTION"
    body = " ".join(r["body"].split())
    if len(body) > width:
        body = body[:width] + "..."
    return (f"- #{r['id']} [{r['kind']}/{tag}, conf {r['confidence']:.2f}, {r['updated'][:10]}, "
            f"src: {r['source'] or 'n/a'}] {r['title']}\n    {body}")


def log_event(event, **kw):
    try:
        LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(json.dumps(dict(ts=now(), event=event, **kw)) + "\n")
    except Exception:
        pass
