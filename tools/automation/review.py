"""Read-only working-tree review: one command instead of the usual six git calls.

Usage: python tools/automation/review.py [--diff] [--expect PATH ...] [--watch PATH]

Reports branch/HEAD, `git status --short`, modified tracked files, `git diff --stat`,
`git diff --check` (exit code; LF/CRLF notices filtered), and the status of one
watched untracked file (default: .claude/reports/usage_log.jsonl).
--diff prints the full diff; --expect lists the only tracked files allowed to be
modified and exits 1 if anything else differs.

Never writes, stages, commits, resets or cleans anything. Git is run with an
argument array (no shell) and a timeout.
"""
import argparse
import datetime
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NOISE = "LF will be replaced by CRLF"


def git(*args):
    p = subprocess.run(["git", "--no-pager", *args], cwd=ROOT, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=60)
    return p.returncode, p.stdout, p.stderr


def lines(text):
    return [ln for ln in text.splitlines() if ln.strip()]


def watched_status(rel):
    path = ROOT / rel
    if not path.exists():
        return f"{rel}: missing"
    code, _, _ = git("ls-files", "--error-unmatch", rel)
    ignored = git("check-ignore", "-q", rel)[0] == 0
    state = "tracked" if code == 0 else ("git-ignored" if ignored else "untracked")
    st = path.stat()
    mtime = datetime.datetime.fromtimestamp(st.st_mtime).isoformat(timespec="seconds")
    n = sum(1 for _ in path.open("rb"))
    return f"{rel}: {state}, {n} lines, {st.st_size} bytes, modified {mtime}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--diff", action="store_true", help="print the full diff")
    ap.add_argument("--expect", nargs="*", default=None, metavar="PATH",
                    help="only these tracked files may be modified")
    ap.add_argument("--watch", default=".claude/reports/usage_log.jsonl")
    a = ap.parse_args()

    code, out, err = git("rev-parse", "--abbrev-ref", "HEAD")
    if code != 0:
        print("git failed:", err.strip())
        return 2
    print(f"branch: {out.strip()}  HEAD: {git('rev-parse', '--short', 'HEAD')[1].strip()}")

    print("\n== git status --short")
    print(git("status", "--short")[1].rstrip() or "(clean)")

    changed = lines(git("diff", "--name-only")[1])
    print("\n== modified tracked files")
    print("\n".join(changed) or "(none)")

    print("\n== git diff --stat")
    print(git("diff", "--stat")[1].rstrip() or "(no diff)")

    code, out, err = git("diff", "--check")
    notes = [ln for ln in lines(out + "\n" + err) if NOISE not in ln]
    print(f"\n== git diff --check: exit {code}")
    print("\n".join(notes) if notes else "no whitespace errors")

    print("\n== watched file (never modified by this script)")
    print(watched_status(a.watch))

    rc = 1 if code else 0
    if a.expect is not None:
        expected = {p.replace("\\", "/") for p in a.expect}
        extra = sorted(set(changed) - expected)
        missing = sorted(expected - set(changed))
        print("\n== expected-files check")
        print("OK: modified files match" if not extra and not missing else
              f"UNEXPECTED modified: {extra or '-'}; expected but unmodified: {missing or '-'}")
        rc = rc or (1 if extra or missing else 0)

    if a.diff:
        print("\n== git diff")
        print(git("diff")[1].rstrip())
    return rc


if __name__ == "__main__":
    sys.exit(main())
