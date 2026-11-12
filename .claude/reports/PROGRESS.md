# Local RAG memory — progress

**Overall: ~85%** (MVP working and tested; live-session behaviour not yet observed.)

## Milestones
- [x] Inspect `.claude/`, Claude Code 2.1.322, hook/skill schema (checked against the docs via the claude-code-guide agent)
- [x] SQLite + FTS5 store, status/confidence/source/timestamp/supersession model, secret redaction
- [x] Scripts: index, retrieve, detect repetition, save, verify, usage report (+ `mem-*.bat` wrappers)
- [x] Hooks: `SessionStart` + `UserPromptSubmit` in `.claude/settings.json` (no prior settings existed, nothing overwritten)
- [x] Skills: `task-memory`, `task-learning`
- [x] Tests: 19 unit/integration tests
- [ ] **Current:** confirm in a fresh Claude Code session that the hooks fire and the injected context appears (needs a restart; not done here)
- [ ] Tune thresholds on real task history once records accumulate

## Tests
`python -m unittest discover -s .claude/tests` → 19 run, 19 OK. Covers duplicate, related-not-duplicate, unrelated, relevance, supersession, update-by-key, failed/assumption labels, empty memory, missing/corrupt DB, secret exclusion, no-network, hook silence and hook failure (always exit 0).

## Known limitations
- Retrieval scans all records and scores token overlap (FTS5 table is maintained but not used for candidate selection); fine to a few thousand records.
- Lexical only: no embeddings. Paraphrases with different vocabulary can be missed; a small synonym map (check/verify/validate...) helps.
- Thresholds are heuristic; "duplicate" is a prompt to re-verify, never to skip work.
- Initial DB holds only the 50 sections from `.claude/memory/*.md` (confidence 0.6). No real task records have been saved yet.
- `memory.db` is git-ignored by the existing `*.db` rule; the JSON mirrors in `memory/task_records|decisions|procedures` are the committable source and rebuild the DB via `index_memory.py`.
- Hooks need a Claude Code restart / `/hooks` review before they take effect; I did not run a live session.
- A `Stop` hook auto-saves each substantial turn (answer >= 400 chars) as an UNVERIFIED assumption (conf 0.4, keyed by prompt fingerprint so repeats update one record). It cannot know whether work was verified; promote good ones with the task-learning skill. Tests: 20/20 OK.

## Credit-saving opportunities (estimates, NOT measured)
Repeated verification audits, repo/test-command rediscovery, re-reading the large `.claude/memory` docs. Each prompt adds at most ~3.5k chars of context only when a match exists. No savings are claimed until `usage_report.py` events can be compared against `/usage` over several sessions.
