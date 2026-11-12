---
name: task-memory
description: Retrieve previous task outcomes, verified commands, decisions and procedures from the local memory database before starting substantial work, repeating an audit, or re-discovering repo facts. Use when a task may repeat earlier work, or when injected "Local memory" context needs more detail.
---

# task-memory

Local-only retrieval (SQLite + FTS in `.claude/memory/memory.db`). No network.

1. Check for repetition: `python .claude/scripts/detect_repetition.py "<task in one sentence>"`
   - `DUPLICATE`: read the matched record, then re-verify only what could have changed since its date (e.g. `git log --since`).
   - `RELATED`: reuse the procedure/decision, adapt it.
   - `NEW`: proceed normally.
2. Look something up: `python .claude/scripts/retrieve_memory.py "<query>"` (add `--all` to also see SUPERSEDED/outdated records).
3. If the DB is empty or stale: `python .claude/scripts/index_memory.py`.

Rules:
- Treat statuses literally: VERIFIED/APPROVED/COMPLETED are established; ASSUMPTION is unverified; FAILED = do not repeat; SUPERSEDED = outdated, never current.
- Memory is history, not truth. Before acting on a recorded fact about code, confirm it against the current source. Owner-approved decisions in `.claude/CLAUDE.md` always win over memory.
- Do not skip verification or reasoning because a record exists; use records to avoid rediscovery, not to avoid checking.
