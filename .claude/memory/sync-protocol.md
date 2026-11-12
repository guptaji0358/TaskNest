# Sync protocol (summary)

Normative: `api/SYNC.md` (protocol v1; section numbers are referenced by `api/conformance/` fixtures,
do not renumber). Wire format: `api/openapi.yaml`. Reasoning: `CLOUD.md`. This file is a map only.

## Vocabulary (SYNC §1)

- **rev**: per-entity revision; task starts at 1, +1 per applied change; never decreases or restarts.
- **seq**: per-account change sequence assigned by the server inside the same transaction; gap-free for applied changes.
- **Tombstone**: server record of a deletion; keeps id, rev, seq, timestamps, reason; no task content; kept for account life.
- **Base**: last server copy adopted by this device, full snapshot (never digests). Written only from server copies.
- **Outbox**: local journal of unacknowledged operations.

## Section map

| Section | Topic |
|---|---|
| §2 | Synced fields (2.1), field groups (2.2), settings (2.3). |
| §3 | Operations, types, deliberate-edit definition (3.2), validation (3.3), server clamps (3.4). |
| §4 | Push: batches and idempotency (4.0), create (4.1), update (4.2), result statuses (4.3), ambiguous outcomes (4.4). |
| §5 | Pull: snapshot and high-water mark (5.1), paging (5.2). |
| §6 | Conflicts: different groups merge (6.1), same group later arrival wins and loser kept (6.2), records and notices (6.3). |
| §7 | Deletion, resurrection, stale delete, presence, restore, tombstone lifetime. |
| §8 | Account-wide cleanup policy. |
| §9 | Isolation, first link, relink, epoch change, sign-in to another account. |
| §10 | Server epoch (restore on server → client re-baseline). |
| §11 | Client rules: recording (11.1), outbox rows (11.2), undo (11.3), compaction (11.4), applying pulls (11.5), when to sync (11.6), push recovery (11.7), held tasks (11.8), tests (11.9), base snapshots (11.10), atomic ack (11.11), local conflict records (11.12). |
| §12 | Limits and errors. |
| §13 | Versioning and compatibility. |

## Core rules (one line each)

- Edit wins over delete both ways; a stale delete is rejected; duplicate deletes are no-ops.
- Different field groups merge; same group keeps the later arrival and stores the overwritten value as a conflict.
- Server clamps `created_at`/`completed_at`; client clocks never decide ordering.
- A push response is applied in one SQLite transaction (11.11).
- Sync is off or inert until an account is linked; no polling; debounced push (≥10 s).

## Where the code lives

- Client rules: `scripts/src/syncstore.cpp`, `scripts/include/syncstore.h`; tests `tests/tst_sync.cpp`.
- Server: M2 has no sync endpoints yet (see [cloud-and-server.md](cloud-and-server.md)).
- Fixtures: `api/conformance/client_cases.json`, `api/conformance/server_scenarios.json`.
