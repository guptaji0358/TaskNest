---
name: task-learning
description: Save a verified task outcome, owner-approved decision, reusable procedure (e.g. a test or verification command), or failed attempt to local memory after substantial work is finished and checked. Use at the end of a task, or when the user says to remember something.
---

# task-learning

Save only what was actually verified (or clearly label it otherwise). Never save secrets, tokens, credentials, env vars, or database contents (the tool redacts obvious ones, but do not rely on that).

```
python .claude/scripts/save_task.py --title "<one-line task/decision>" --body "<outcome, exact commands, key facts>" \
  --kind task|decision|procedure|fact --status verified|approved|completed|failed|assumption|pending \
  --source "<file, command, or commit>" --keywords "<extra search words>" [--supersedes <old id>]
```

- `approved` only for decisions the owner explicitly made in chat.
- When a newer result replaces an old record, pass `--supersedes <id>` so the old one stops being retrieved as current.
- Record failed attempts with `--status failed` and why they failed.
- Do not save routine edits or things derivable from the code or git history.
- Ask the user before saving if unsure the result is verified.
