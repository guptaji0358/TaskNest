"""Commit staged changes and push, or open a PR. Usage:
  ship.py push -m "message"   commit what is STAGED (never git add) and push current branch
  ship.py commit -m MSG --paths P [P ...] [--no-push] [--branch B] [--log short|long|none]
  ship.py commit --plan plan.json [--no-push] ...   several commits: [{"message": "...", "paths": ["..."]}, ...]
  ship.py pr [-t TITLE] [-b BODY] [--base main] [--draft]   open a PR for the current branch
`commit` stages ONLY the named root-relative paths (never ".", -A, absolute, ".." or glob paths), requires an
empty index first, makes one commit per group, then pushes once (unless --no-push).
Refuses: main/master, nothing staged, staged *.db/*.sqlite*/*.exe/*.o/out/ files.
"""
import argparse, json, os, re, subprocess, sys

BAD = re.compile(r"(\.db$|\.sqlite|\.exe$|\.o$|\.dll$|^out/)", re.I)


def git(*a, check=True):
    r = subprocess.run(["git", *a], capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(f"git {' '.join(a)} failed:\n{r.stderr.strip()}")
    return r.stdout.strip()


def branch():
    b = git("rev-parse", "--abbrev-ref", "HEAD")
    if b in ("main", "master", "HEAD"):
        sys.exit(f"Refusing on '{b}'. Switch to a feature branch.")
    return b


def push(a):
    b = branch()
    staged = git("diff", "--cached", "--name-only").splitlines()
    if staged:
        bad = [f for f in staged if BAD.search(f)]
        if bad:
            sys.exit("Refusing, staged files not allowed:\n  " + "\n  ".join(bad))
        print("Staged:\n  " + "\n  ".join(staged))
        if not a.message:
            sys.exit("Need -m MESSAGE to commit.")
        git("commit", "-m", a.message)
    else:
        print("Nothing staged; pushing existing commits only.")
    subprocess.run(["git", "push", "-u", "origin", b], check=True)


def _clean_paths(paths):
    root = os.path.realpath(git("rev-parse", "--show-toplevel"))
    out = []
    for p in paths:
        q = p.replace("\\", "/")
        if (not q or q in (".", "-A", "--all") or q.startswith(("-", "/", ":")) or re.match(r"^[A-Za-z]:", q)
                or ".." in q.split("/") or any(c in q for c in "*?[")):
            sys.exit(f"Refusing path {p!r}: use files/folders relative to the repo root (no '.', -A, globs, '..').")
        if not os.path.realpath(os.path.join(root, q)).startswith(root + os.sep):
            sys.exit(f"Refusing path {p!r}: outside the repository.")
        out.append(q)
    if not out:
        sys.exit("Need at least one path.")
    return out


def commit(a):
    b = a.branch or branch()
    if b in ("main", "master", "HEAD"):
        sys.exit(f"Refusing on '{b}'.")
    if a.plan:
        with open(a.plan, encoding="utf-8") as f:
            groups = json.load(f)
    else:
        if not a.message:
            sys.exit("Need -m MESSAGE (or --plan).")
        groups = [{"message": a.message, "paths": a.paths or []}]
    if not isinstance(groups, list) or not groups:
        sys.exit("Plan must be a non-empty list of {message, paths}.")
    for g in groups:
        if not isinstance(g, dict) or not str(g.get("message", "")).strip():
            sys.exit("Every group needs a non-empty message.")
        g["paths"] = _clean_paths(g.get("paths") or [])
    if git("diff", "--cached", "--name-only"):
        sys.exit("Refusing: the index already has staged files; unstage them so each commit holds only its paths.")
    for g in groups:
        git("add", "--", *g["paths"])
        staged = git("diff", "--cached", "--name-only").splitlines()
        bad = [f for f in staged if BAD.search(f)]
        if bad:
            git("reset", "-q", check=False)
            sys.exit("Refusing, staged files not allowed:\n  " + "\n  ".join(bad))
        if not staged:
            sys.exit(f"STOP: nothing to commit for {g['paths']}; earlier groups (if any) are already committed.")
        git("commit", "-m", g["message"])
        if a.log != "none":
            print(git("log", "-1", "--stat" if a.log == "long" else "--oneline"))
    if a.no_push:
        print("Committed; not pushed (--no-push).")
    else:
        subprocess.run(["git", "push", "-u", "origin", b], check=True)


def pr(a):
    b = branch()
    cmd = ["gh", "pr", "create", "--base", a.base, "--head", b]
    cmd += ["--title", a.title, "--body", a.body or ""] if a.title else ["--fill"]
    if a.draft:
        cmd.append("--draft")
    subprocess.run(cmd, check=True)


p = argparse.ArgumentParser()
s = p.add_subparsers(dest="cmd", required=True)
x = s.add_parser("push"); x.add_argument("-m", "--message"); x.set_defaults(f=push)
c = s.add_parser("commit"); c.add_argument("-m", "--message"); c.add_argument("--paths", nargs="+")
c.add_argument("--plan"); c.add_argument("--no-push", action="store_true"); c.add_argument("--branch")
c.add_argument("--log", choices=["short", "long", "none"], default="short"); c.set_defaults(f=commit)
y = s.add_parser("pr"); y.add_argument("-t", "--title"); y.add_argument("-b", "--body")
y.add_argument("--base", default="main"); y.add_argument("--draft", action="store_true"); y.set_defaults(f=pr)
a = p.parse_args(); a.f(a)
