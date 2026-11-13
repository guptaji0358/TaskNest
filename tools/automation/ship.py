"""Commit staged changes and push, or open a PR. Usage:
  ship.py push -m "message"   commit what is STAGED (never git add) and push current branch
  ship.py pr [-t TITLE] [-b BODY] [--base main] [--draft]   open a PR for the current branch
Refuses: main/master, nothing staged, staged *.db/*.sqlite*/*.exe/*.o/out/ files.
"""
import argparse, re, subprocess, sys

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
y = s.add_parser("pr"); y.add_argument("-t", "--title"); y.add_argument("-b", "--body")
y.add_argument("--base", default="main"); y.add_argument("--draft", action="store_true"); y.set_defaults(f=pr)
a = p.parse_args(); a.f(a)
