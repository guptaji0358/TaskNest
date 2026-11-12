"""Usage report from the local event log. Reports counts only; makes NO claim of measured credit savings.
Usage: python usage_report.py"""
import json, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tnmem as m

c, verdicts = Counter(), Counter()
if m.LOG_PATH.exists():
    for line in m.LOG_PATH.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            e = json.loads(line)
        except ValueError:
            continue
        c[e.get("event")] += 1
        if e.get("event") == "prompt_hook":
            verdicts[e.get("verdict")] += 1
print("events:", dict(c))
print("prompt-hook verdicts:", dict(verdicts))
print("injected context chars (total):", sum(
    json.loads(l).get("chars", 0) for l in m.LOG_PATH.read_text(encoding="utf-8").splitlines() if l.startswith("{")
) if m.LOG_PATH.exists() else 0)
print("NOTE: duplicate/related hits show where rediscovery MAY have been avoided; actual token/credit savings are "
      "not measured here (compare /usage before and after over several sessions).")
