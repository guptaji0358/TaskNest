# CLOUD.md — TaskNest cloud sync: architecture decision record

Status: **M0 (contract, rules, fixtures, documentation) is complete; the owner's review raised eight issues (addressed in M0 revision 2, section 13) and then three more (base advancement, local conflict storage, atomic acknowledgement; addressed in M0 revision 3, section 14). M1 is NOT approved and nothing has been implemented.** No application source, database schema, dependency or build file was changed by M0. The "Add Cloud" feature is opt-in; with it off, TaskNest is unchanged (local SQLite, no account, no network).

Where things live:

| File | Purpose |
|---|---|
| `CLOUD.md` (this file) | Decisions, reasoning, risks, roadmap |
| `api/openapi.yaml` | The wire contract (OpenAPI 3.1, REST + JSON, `/v1`) |
| `api/SYNC.md` | Normative sync, conflict and deletion rules (numbered sections) |
| `api/conformance/` | Executable examples shared by the Qt and server tests |
| `CLAUDE.md` → "Optional Cloud Sync" | The invariants every milestone must obey |

## 1. Repository findings (basis for the design)

| Area | Finding | Consequence |
|---|---|---|
| Schema | `user_version` is 3. Migrations run in one transaction after a byte-copy to `tasknest.db.v<N>.bak`. A newer-schema database is refused. | A v4 migration fits the existing pattern. |
| Task identity | `id INTEGER AUTOINCREMENT`. It appears in deep links (`tasknest://task/<id>`), the integration API, the widget and undo-restore. | Keep `id` local; add a UUID alongside it. |
| Mutations | All writes go through `TaskRepository`: add, update, setCompleted, delete, restore, purgeCompleted, markReminderSent. Most are single statements with no transaction. | One chokepoint for the outbox. Each mutation needs a transaction. |
| Deletes | Hard `DELETE` in `deleteTask` and `purgeCompleted` (also used by the "Immediately" cleanup). | Sync needs tombstones. |
| Threading | One `QSqlDatabase` connection on the UI thread. No WAL (only `foreign_keys` is set). | Run the sync engine on the main thread over async `QNetworkAccessManager`, with small transactions. WAL is avoided for now. |
| Backup | Only the upgrade `.bak`. | Extra byte-identical backups before linking and before sync-driven deletion. |
| Android | 19 files use Win32. The CMake target is Windows-only. The reminder scheduler uses `QTimer`. | Core must become a platform-neutral library; reminders need `AlarmManager`. |
| Tests | About 135 test functions on temporary databases, plus a QML UI suite. | Good base for sync tests. |
| Qt | Build cache, `CLAUDE.md` and `rebuild-and-run.bat` use 6.11.1, the only Qt installed (MinGW and MSVC kits, no Android kit). Nothing in the repo mentions 6.8; CMake requires 6.5 or newer. | See decision 1 and risk R1. |

## 2. Recommendation

**FastAPI + PostgreSQL** with a TaskNest-owned REST contract, as proposed. Changes to the proposed stack:

- Share conformance fixtures between pytest and Qt Test instead of sharing code. This removes the main argument for a C++ backend.
- Neon's pooled endpoint is PgBouncer in transaction mode: set asyncpg `statement_cache_size=0` there and run Alembic against the direct endpoint.
- Order changes with a **per-account counter row** locked inside the push transaction, never a global Postgres SEQUENCE (a transaction holding seq 5 can commit after seq 6 is visible, so a client at cursor 6 would miss 5).

| | FastAPI | Drogon (C++) | Go (net/http + pgx) | Supabase / PocketBase / sync engines |
|---|---|---|---|---|
| Maintainability | High | Low (async C++ boilerplate) | High | Highest at first |
| Security | Mature libraries | Hand-rolled auth in memory-unsafe code exposed to the internet | Good | Vendor-managed |
| Ecosystem | Largest | Small | Large | Not yours |
| Portability | Docker, anywhere | Docker, anywhere | Single small binary | Poor (vendor tie-in) |
| Dev cost | Low | High | Medium (third language) | Lowest |
| Later migration cost | Low (contract is HTTP) | Low | Low | High |

**A (FastAPI) is recommended. B (Drogon) is rejected.** Go is the only genuine alternative (low memory on free tiers) but is not worth a third language. Vendor sync engines and CRDTs (Automerge, Yjs) are overkill for flat task fields and break vendor independence.

## 3. Component architecture

```
Qt app (desktop / Android)
 ├─ UI (QML) ── SyncController (status, toggle, errors)
 ├─ tasknestcore  (new platform-neutral library: Core, Sql, Network)
 │    ├─ Database / TaskRepository (outbox written in the same transaction)
 │    └─ SyncEngine ── SyncTransport (interface) ── HttpTransport (QNetworkAccessManager)
 └─ Secrets store (Windows Credential Manager / Android Keystore)

TaskNest API (FastAPI, Docker) ── PostgreSQL (any standard host)
```

Monorepo: `api/` (exists now), `server/` (created at M2) beside `scripts/`; backend dependencies never enter the C++ build; server virtual environment and caches live under `out/server/`.

## 4. Decisions

| # | Decision | Where it lands |
|---|---|---|
| 1 | **Qt 6.8 is the minimum supported version; 6.11.1 stays the development version.** No Qt upgrade or switch for the cloud work without separate approval. | CLAUDE.md; risk R1 |
| 2 | **No paid domain initially.** The server URL is a client setting and the contract has no host names, so a domain can be added later without a contract change. | Section 8 |
| 3 | **Recovery codes in v1, no email provider.** Ten single-use codes of 80 bits, shown once, only keyed hashes stored. Losing both password and codes makes the cloud account unrecoverable. | Section 7 |
| 4 | **Retention purges converge across devices** through synced tombstones. | SYNC 7 |
| 5 | **Edit wins over delete**, with a notice and preserved conflict information. | SYNC 7.3, 6.3 |
| 6 | **Monorepo.** `api/` and `server/` beside the Qt app; isolated dependencies. | CLAUDE.md |
| 7 | **A deliberate edit resurrects a purged task; mere presence of an old copy never does.** An explicit, identifiable operation (`update`, `origin: user`, non-empty `changed`, unique `op_id`) is validated against the server's deletion state (`base_rev` below the tombstone's rev). | SYNC 3.2, 7.3, 7.5 |
| 8 | **One account-wide cleanup policy when sync is on.** All linked devices follow it; the local policy is never overwritten, so local-only use stays independent. | SYNC 8 |
| 9 | **No automatic account deletion in v1.** Accounts are removed only on the user's request. Inactivity deletion may be reconsidered later with advance notice and a recovery period. | Section 7 |
| 11 | **Bases are full snapshots, not digests.** The before-value is needed to show and restore what changed; the cost is one task copy per synced task. | SYNC 1, 11.10, 11.12 |
| 10 | **Local PostgreSQL installation for development and integration tests.** No Docker Desktop requirement. Production and test databases are completely separate. PostgreSQL is not installed without separate approval. | CLAUDE.md; M2 |
| 12 | **M2 / M3 boundary: M2 contains no sync tables.** M2 is accounts, sessions, recovery and devices only. `processed_ops`, `conflicts` and Alembic migration 0002 belong to M3, and so do the retention purge for them and its automated database tests. M2 documents the 90-day policy and prepares the design only. | Sections 5, 10, 16 |
| 13 | **Retention disclosure.** Processed operations and conflict records are kept 90 days. Conflict records can hold task content, so **deleted task content can remain in conflict history for up to 90 days.** The sync privacy notice (M5) must say so. | SYNC 6.3 (proposed wording, section 16.5) |
| 14 | **Password policy (accounts).** NFC-normalized, 15 to 128 Unicode code points (counted after normalization), case and whitespace preserved, no composition rules, no periodic change. A local common-password blocklist is applied (section 7). Login identifiers use NFKC + casefold. | Section 7 |
| 15 | **Session lifetime.** Refresh tokens slide 90 days and end at 180 days after the token family was created, with rotation and reuse detection. Rotation is **strict (design A, approved)**: no grace window, no stored successor, a lost refresh response means re-login (sections 16.4, 16.7). Every refresh carries a client `operation_id`; the only exception to "reuse revokes the family" is a retry of the very request that rotated the token, which gets `409 refresh_superseded` without revocation (16.7). A family allows at most six successful rotations per rolling 15 minutes. | Sections 7, 16.7 |
| 16 | **Device cap.** At most 25 active devices per account. A sign-in over the cap is refused with `409 device_limit_reached` unless the user explicitly names one of their own active devices in `replace_device_id` (16.8); no device is ever revoked automatically. Device list and revoke are part of M2. | Sections 7, 16.8; SYNC (proposed) |

## 5. Design summary

Normative detail is in `api/SYNC.md`; this is the short form.

- **Local schema v4 (additive):** `tasks.uuid` (unique, backfilled in the migration transaction), `tasks.server_rev` (null = never synced; always equal to the base's rev) and the task's **base** (`sync_base`: the last server copy as a **full snapshot**, never digests; SYNC 1, 11.10), per-task `held` / `not_synced` flags (SYNC 11.8), a `sync_tombstones` table instead of a `deleted_at` column (so no existing query needs a new filter), `sync_outbox` (rows have `state`, `queued_at`, `attempts`, `authored_rev`, `collided`, `restore_of`, the frozen `sent_payload` and an `after` link; SYNC 11.2, 11.7), `sync_conflicts` (complete logical schema, restore, retention and cleanup rules in SYNC 11.12), and sync state in `app_metadata` (including a link-generation counter). A push response is applied in **one** SQLite transaction (SYNC 11.11). Outbox and tombstones are recorded only once an account is linked; before that TaskNest is unchanged and accumulates nothing.
- **Server:** `users`, `devices` (every sign-in creates one), `refresh_tokens` (hashed, with a family id), `recovery_codes` (keyed hashes), a per-account sequence counter, `tasks` (composite key account + UUID, rev, seq, per-group revisions and devices, tombstone fields), `settings`, `processed_ops` (≥ 90 days), `conflicts` (90 days). **M2 creates only the account tables** (`users`, `devices`, `refresh_families`, `refresh_tokens`, `access_tokens`, `recovery_codes`; section 16.6); the sync tables (`tasks`, `settings`, the sequence counter, `processed_ops`, `conflicts`) arrive with M3 (decision 12).
- **Change model:** state-based pull by sequence number, latest state per entity, tombstones included and kept for the life of the account (content removed at deletion). Pull cursors advance only through pull.
- **Merging:** per field group (`title`, `notes`, `list_name`, `priority`, `due_at`, `reminder_at`, and `completed` + `completed_at` as one group). Different groups merge. For the same group the later arrival wins and the overwritten value is stored as a conflict record and delivered to the losing device. `notes` is not text-merged in v1.
- **Deletes:** edit wins over delete both ways. A stale delete is rejected (`edit_won`); a stale edit resurrects the task. Duplicate deletes are no-ops.
- **Time:** the server alone orders changes. `created_at` and `completed_at` are clamped to the server clock because retention uses them. Due dates remain UTC instants, so "due 23:59 local" shifts across time zones.
- **Sync triggers:** debounced push (at least 10 s, longer than the 8 s undo window), pull on window activation, "Sync now", single-shot exponential backoff only while there is work. No polling.
- **Epoch:** `server_epoch` changes if the server database is restored; clients then re-baseline (back up, discard outbox and cursor, compare with the server, never resurrect, never overwrite silently).

## 6. API summary

Seventeen operations under `/v1` (full detail in `api/openapi.yaml`): `GET /health`, `GET /version`; `POST /auth/register`, `/auth/login`, `/auth/refresh`, `/auth/logout`, `/auth/recover`, `/auth/recovery-codes`, `/auth/password`; `GET /devices`, `DELETE /devices/{id}`; `POST /sync/push`, `GET /sync/pull`, `GET /sync/state`, `GET /sync/conflicts`; `GET /export` (NDJSON); `POST /account/delete`. There is no separate "create device" call (sign-in creates it) and no import endpoint (restore from an export file is a client feature that sends ordinary operations).

## 7. Authentication and security

- **Passwords:** Argon2id at OWASP-minimum or higher parameters. On a 512 MB free instance, hashing is bounded by a semaphore (approved Option B, 16.11): Argon2id `m=19456` KiB, `t=2`, `p=1`; two concurrent hashes, six waiting, a 2-second acquisition timeout answered `503 server_busy`; one Uvicorn worker.
  - **Policy (decision 14):** applied to the NFC-normalized password. Minimum 15 and maximum 128 Unicode code points (counted after NFC). Case and whitespace are preserved (nothing trimmed or collapsed). No composition rules and no periodic changes. Passwords are normalized with **NFC**; login identifiers with **NFKC + casefold**. The 15-character minimum follows NIST SP 800-63B-4 for a password used as the only authenticator (recovery codes are a reset path, not a second factor). The exact standard wording is quoted when the server policy document is written (M2.2).
  - **Blocklist:** a list of common and compromised passwords stored locally with the server and checked in memory at registration, change and recovery. No registration-time external API and no network call. Rejected also: the login identifier itself (as submitted and normalized, with common case and trivial-substring variants) and the product name `TaskNest`. Before it is added, the server documentation records the list's **source, licence, version and update procedure** (a reviewed file replaced by a commit, never fetched at run time). Tests cover: a listed password refused, a context value refused, an unlisted password accepted, and the list loading from the packaged file. Adding the list is a data dependency that is approved with M2.2.
  - Existing passwords are only normalized at login, never re-checked against the policy, so a later policy change cannot lock anyone out.
- **Tokens:** 15-minute opaque access token. Refresh tokens are opaque 256-bit values, stored hashed, bound to a device, rotated on every successful refresh; reuse of a rotated token revokes the whole family, except a retry of the same refresh operation (section 16.7).
  - **Lifetime (decision 15):** a refresh token slides 90 days from each rotation, but never beyond **180 days after the family was created**. Past either limit the client is answered `401` and signs in again; local data is untouched.
  - **Interrupted rotation (design A, approved):** if the client never stored the new token (crash, dropped connection), the raw successor cannot be recovered. Retrying the same request (same `operation_id`) is answered `409 refresh_superseded` and revokes nothing; presenting the old token in any other request counts as reuse and revokes the family. Either way the user signs in again. No grace window; no successor tokens are stored (precedence table in section 16.7).
  - **Access tokens follow the family:** an access token authorizes a request only while its device's refresh-token family is neither revoked nor expired. The server checks this on every request and never issues an access token that outlives its family, so an expired or revoked device loses access at once and cannot keep a slot's worth of valid access. Every login creates a new device record (no client-held device ids in v1).
- **Devices (decision 16):** at most **25 active devices**. A device is active while its refresh-token family is neither revoked nor expired. Sign-in over the cap returns `409 device_limit_reached` with `details {limit, active}`; no session is created and nothing is revoked silently. The user either revokes a device (`DELETE /v1/devices/{id}`) and signs in again, or signs in with `replace_device_id` naming an active device to replace (section 16.8). Recovery revokes every session, so it frees all slots.
- **Client storage:** Windows Credential Manager and Android Keystore, never SQLite, settings, logs or command lines. A Windows credential blob is limited to 2560 bytes, which these tokens fit. A keychain library would be a third-party dependency and needs approval; Win32 `CredRead`/`CredWrite` do not.
- **Isolation:** every identifier is scoped to the token's account; another account's id behaves exactly like an unknown id. Server tests must try cross-account access on every endpoint. Row-level security is not relied on (not portable).
- **Recovery codes:**
  - Ten single-use codes, 16 base32 characters (80 bits), returned once at registration and on regeneration.
  - Stored as HMAC-SHA256 with a server-side pepper from the environment plus a non-secret code id. There is **no `recovery_codes.pepper_id` column**. A slow hash adds nothing at this entropy; the pepper protects against a stolen database.
  - Constant-time comparison; rate limits per account and per address.
  - Using a code sets a new password and revokes every session and token family. Local data on devices is untouched. A code is consumed atomically under the `users` lock after a re-read confirms it is still unused, so concurrent requests cannot both succeed with the same code (16.11).
  - If password and all codes are lost, the account cannot be recovered: there is no email and no operator override. Each device still holds a full local copy, so the user creates a new account and uploads from a device (SYNC 9.2). The old cloud account remains until the user deletes it (decision 9). The UI must explain this at registration, before the codes can be dismissed.
- **Login identifier:** an unverified login name (email format allowed, never mailed). Registration answering "taken" allows login-name enumeration; this is accepted for v1 and mitigated by rate limits.
- **Abuse limits:** TLS only, rate limits on authentication and sync, no task content in logs, payload and field limits (SYNC 12). The per-address limit and trusted-proxy rules are in section 16.9 (endpoint scope **proposed, not approved**).
- **End-to-end encryption:** not in v1 (it would break server-side features). Merging works on opaque field values, so it can be added later with a protocol revision.

## 8. Provider portability

- Only standard PostgreSQL: a `DATABASE_URL`, plain SQL, UUIDs generated in application code. No extensions, triggers or provider SDKs.
- One Dockerfile and 12-factor environment configuration. Local development and tests use a locally installed PostgreSQL (decision 10), so the container is for deployment only.
- Alembic runs as a release step against the direct (non-pooled) connection.
- The client's server URL is a setting. **Without a domain the provider hostname is the server's identity.** If that service is deleted the hostname cannot be reclaimed, so moving hosts means each client edits its URL by hand. The settings screen must allow that and the runbook must say so. A custom domain can be added later without changing the contract.
- Optional relocation hint: `GET /version` may return `moved_to`. A client never switches silently: it shows the new HTTPS URL and asks. Redirects are not followed with credentials across hosts.
- Move rehearsal (M8): `pg_dump` and restore to a second host and repoint the URL. `server_epoch` protects clients if the dump is older than their cursor.
- Every client holds a full replica, so a lost server can be rebuilt from a client. Client restore targets an empty database, or happens after a fresh backup.

## 9. Free-tier assessment

Limits below are from last-known information and change often. **Verify each before committing.**

| Service | Typical free limits | Fit |
|---|---|---|
| Neon Postgres | About 0.5 GB, scales to zero (first query after idle takes a second or two), limited compute hours, short point-in-time recovery window | Good for personal use |
| Render web service | 512 MB RAM, sleeps after about 15 minutes idle, cold start up to a minute | Works because sync is background. Do not use Render's free Postgres (it expires). |
| Koyeb | Free-tier terms have changed before | Check current terms |
| Fly.io | No longer free for new accounts | Skip |

Suited to the owner and a few users, not an SLA. The client uses a 90-second first-request timeout and backoff with jitter, shows "server waking up", and never writes keep-alive pings. Triggers to pay: more than a handful of users or a need for uptime; the portable end state is a small VPS (about €4–5/month) running Docker with Postgres and a reverse proxy. Tombstones are kept forever but hold no content (roughly 100–200 bytes per deleted task), which is small against 0.5 GB; the server should alert on database size.

## 10. Staged roadmap

Each milestone starts only after the owner's explicit approval.

| Stage | Deliverable | Testable exit criteria | Status |
|---|---|---|---|
| **M0** Contract | `openapi.yaml`, `SYNC.md`, conformance fixtures, CLAUDE.md amendment, this record | Contract parses and its references resolve; fixtures consistent with the rules and with the schemas (sections 11, 13, 14) | **Done (revision 3 after review), awaiting owner decision** |
| **M1** Local prep (no network) | Schema v4, UUIDs, bases, tombstones, outbox, transactional mutations | Migration keeps every row (tested on copies of real databases); bases are full snapshots written only from server copies; the response-acknowledgement transaction and the conflict table follow SYNC 11.10 to 11.12; a failed mutation leaves no outbox row; every **synced** mutation path (add, update, setCompleted, delete, restore, purge, "Immediately", "Clear completed") writes exactly one operation or folds into an unsent row; **`markReminderSent` is local-only: it writes no outbox row and leaves `server_rev` and the base unchanged**; undo of a sent or in-flight delete covered; client fixtures for the outbox, apply-pull and local-only cases pass in Qt Test | **Approved by the owner (recorded in CLAUDE.md)** |
| **M2** Server skeleton (**accounts only, no sync tables**) | FastAPI, Dockerfile (file only), Alembic baseline (account tables), register / login / refresh / logout / recover / regenerate codes / change password, device list and revoke, 25-device cap, password policy and blocklist | Register, login, rotating refresh with sliding 90 / absolute 180-day lifetime, recovery work; token reuse revokes the family; the 26th device is refused with `device_limit_reached` and nothing is revoked; revoke frees a slot; cross-account access refused on every endpoint; password policy and blocklist tests pass; needs local PostgreSQL (separate approval). Sub-steps in section 16. | **M2.0 (documentation) approved; M2.1 onward not approved** |
| **M3** Sync endpoints | Migration 0002 (`tasks`, `settings`, sequence counter, `processed_ops`, `conflicts`), push, pull, idempotency, conflicts, tombstones, **90-day retention purge for `processed_ops` and `conflicts` with its automated database tests** | Server scenarios pass; replayed batches change nothing; three simulated devices converge under random interleavings; concurrent pushes never skip a seq; purge deletes at 90 days and not before, never removes a row still referenced, and is idempotent | Not approved |
| **M4** Client engine | `SyncTransport`, drain, cursor, backoff | Killing mid-push never duplicates; the same fixtures pass in Qt Test | Not approved |
| **M5** Desktop UI | Add Cloud toggle, sign-in, recovery codes screen, status, Sync now, first-link flow with backup | `tst_ui` passes; backup exists before the first push; tokens are in the credential store | Not approved |
| **M6** Triggers | Debounced push, pull on activation, cold-start handling | Works across a network drop; no polling loops | Not approved |
| **M7** Data ownership | Export, cloud delete, restore, epoch reset (device list and revoke moved to M2) | Export/restore round-trip matches; an old restored dump triggers a safe re-baseline | Not approved |
| **M8** Portability drill | Deploy to a second provider | Repointing the URL works with no client change | Not approved |
| **M9** Android | Core library split, Android kit, reminders, secure storage | Windows and an Android emulator sync both ways; needs a Qt 6.8 Android kit (separate approval) | Not approved |
| **M10** Hardening | Security review, dependency audit, monitoring | No open high-severity findings | Not approved |

M1 is the riskiest for existing data. M9 is the largest.

## 11. M0 results

### Delivered

| Deliverable | Result |
|---|---|
| `api/openapi.yaml` | OpenAPI 3.1, 17 operations, 44 schemas, no provider names |
| `api/SYNC.md` | 13 numbered sections: model, operations, push, pull, conflicts, deletion and resurrection, cleanup policy, isolation and first link, epoch, client rules (11.1 to 11.12, including base snapshots, atomic acknowledgement and local conflict records), limits, versioning |
| `api/conformance/` | 32 server scenarios, 91 client cases, README (24 and 20 at M0 revision 1; 32 and 49 at revision 2) |
| `api/.gitattributes` | Forces LF so both test suites read identical bytes |
| `CLAUDE.md` | Optional-cloud principle, directory rules for `api/` and `server/`, Qt statement, build isolation, "Optional Cloud Sync" section, development rule 8 |
| `CLOUD.md` | This record |

### Validation

Nothing may be installed in M0, and no OpenAPI validator is installed, so validation was done with throwaway scripts outside the repository (standard library plus the PyYAML already present):

- **Contract structure:** YAML parses; OpenAPI version 3.1; 133 `$ref`s all resolve; operation ids unique; path parameters declared; every operation has responses; no provider name appears in the contract; model enums (field groups, delete reasons, setting keys, task fields) equal the schema enums. **PASS.** This is a structural check with a small hand-written JSON Schema subset, not a certified OpenAPI validator. Its self-tests confirm it accepts a valid task and rejects five invalid ones.
- **Rules executed:** a reference model of `SYNC.md` ran all 24 server scenarios and 20 client cases. **44 of 44 pass.** The first run found four wrong expectations (a new task starts at rev 1 even when its seq is higher); they were fixed in the fixtures, not in the rules.
- **Fixtures against the schema:** 85 request operations validated against the `Op` schema and 61 model responses against `PushResponse` / `PullResponse`. **PASS.** One case (a `completed_at` on a not-completed task) is deliberately schema-valid and rejected by a server rule, as `SYNC.md` 3.3 states.
- **Files:** all `api/` files are LF with no BOM.

### What this validation does not show

The reference model and the fixtures were written by the same author from the same understanding, so passing proves they agree with each other, not that the rules are right. The real evidence comes when the server (M3) and client (M4) pass the fixtures independently. Revision 2 added fixtures for in-flight rebasing, ambiguous outcomes, the 60-day rule, held tasks and three-way reconciliation (section 13). Still not covered: the epoch-reset flow end to end, first-link policy behavior, crash points inside a local transaction, and anything involving real time, threads or the network.

## 12. Remaining technical risks

Severity: **High** = can lose or corrupt data, or blocks the plan; **Medium** = costs rework or surprises users; **Low** = housekeeping.

### Resolved by the owner's decisions

| Earlier risk | Resolution |
|---|---|
| R3 edit wins versus "never resurrect purged records" | Decision 7, implemented in SYNC 3.2, 7.3, 7.5 and covered by fixtures |
| R5 cleanup policy scope | Decision 8, SYNC 8 |
| R6 orphaned accounts | Decision 9: no automatic deletion in v1 |
| R11 PostgreSQL for tests | Decision 10: local installation, separate approval to install |

### Open

**R1 (High): Qt 6.8 minimum is unverified.** Only 6.11.1 is installed; no 6.8 kit and no Android kit exist on this machine, and the app has only been built on 6.11.1. Proving 6.8 means installing a 6.8 kit (desktop, later Android) and running the build and tests on it: needs separate approval. Until then "minimum 6.8" is a stated intent, not a fact.

**R2 (High): data safety around first link and sync-driven deletion.** M1 touches every row of the live database (`out/bin/data/`, inside a OneDrive checkout). Back it up outside the repo before M1 runs, test the migration on copies, and keep the byte-identical backup rules in CLAUDE.md. Sync-driven local deletion (applying a tombstone) needs its own backup when it removes many tasks.

**R3 (Medium): arrival-order wins can overwrite newer data.** A stale offline edit that reaches the server last overwrites a newer value on the same field. This was chosen because it is deterministic and clock-independent; recovery is the stored conflict record plus a restore action. `client_edited_at` is carried but never used. If this proves painful, a future protocol revision could use it as a tiebreaker within a skew bound.

**R4 (Medium): `notes` is not merged.** Concurrent note edits keep one version and store the other as a conflict. Acceptable for v1; a three-way text merge is a later option.

**R5 (Medium): "Clear completed" and the account policy act on every device.** With sync on, these are account-wide. The UI must say so in the confirmation text, and the policy-change notice on receiving devices must be clear.

**R6 (Medium): epoch reset can resurrect recent deletions.** After a server restore from an older dump, deletions made after the dump's date may reappear (SYNC 10). Rare; documented.

**R7 (Medium): hostname lock-in without a domain** (section 8). If the free host disappears, every client needs a manual URL change.

**R8 (Medium): free-tier behavior and no forced updates.** Cold starts and pauses must never block local use, and the server must keep serving old protocol versions; an unsupported client stops syncing with a message but works locally.

**R9 (Medium): Android.** Reminders need `AlarmManager`, Android 13 needs the notification permission, background sync needs `WorkManager`, and the 19 Win32 files must be isolated before an Android build exists.

**R10 (Medium): local `updated_at` after a pull.** `updated_at` is not synced. M1 must decide what a pull-applied change writes there, and make sure it does not fire the "reminder re-armed" logic or create an operation.

**R11 (Medium): monorepo hygiene.** `.gitignore` has no Python entries (`.venv`, `__pycache__`, `.pytest_cache`, `.env`); that change belongs to M2 and was not made in M0 (the scope excluded it). Server tooling must be configured to write under `out/server/`.

**R12 (Medium): tooling.** PostgreSQL is not installed (decision 10, approval pending, M2), Docker is not installed and is not required, there is no CI configuration, and no independent OpenAPI validator exists on this machine (adding one is a dependency decision for M2).

**R13 (Low): other.** Login-name enumeration through "taken" answers (rate-limited); due dates as UTC instants shift across time zones; the privacy notice on enabling sync; the held-task UI for "deleted in the cloud while this device was unlinked" must be designed in M5.

## 13. M0 revision 2: owner review findings

The owner reviewed `CLOUD.md` and `api/SYNC.md` and declined to approve M1 until eight issues were investigated. Each was confirmed as a real gap or defect; none changed application code, schemas or dependencies. The reference model was rebuilt (the first one was not kept) and re-ran the original 24 scenarios and 20 cases (one case changed on purpose, below) before the new ones were added.

| # | Finding | Verdict | Resolution |
|---|---|---|---|
| 1 | Re-baseline used the server's current rev as `base_rev` for a possibly stale local task, silently overwriting newer cloud data (and the epoch reset threw away the only information that could tell the two apart) | **Confirmed defect.** The old rebaseline case encoded it. | Every task keeps a **base** (last server copy seen), across unlink, relink and epoch change. 9.2 is now a three-way rule per field group: only groups the device changed and the server did not are sent (on the current rev, which is then safe because the server value equals what the device saw); groups changed on both sides keep the cloud value and the local value is saved in a local conflict record; with no base, nothing is sent and differing groups keep the cloud value with local values preserved. The epoch reset keeps base content and distrusts old revs. |
| 2 | Restore had no defined payload although tombstones hold no content | **Confirmed gap** (the OpenAPI already required `task`; SYNC did not say what to do with it). | 7.6: restore carries the full task, validated like `create` (3.3, including `task.id` = `task_id`), `created_at` forced to the tombstone's stored value, clamped `completed_at`, exact check order and results; tombstones now keep `created_at` server-side. |
| 3 | "5xx means nothing was applied" | **Confirmed unsafe**: operations commit independently. | New 4.4: any non-200 or dropped connection is an unknown outcome; 401/413/422/429 are guaranteed pre-application; stored results include rejections; replay is byte-identical via a frozen `sent_payload`; an op is never re-sent under a new `op_id` (a scenario shows the new copy overwrites a newer value). OpenAPI text corrected. |
| 4 | Rows older than 60 days were undefined | **Confirmed gap.** | 11.6 table: such rows are removed and their intent is reconciled through the 9.2 three-way rule, never re-sent with the old base or id. Deletes, creates and tombstones are covered; a deleted task is held, never silently resurrected. |
| 5 | In-flight recovery, rebasing and held transitions were unspecified | **Confirmed gap.** | 11.2 (row states, `after` link), 11.5 (ignore items older than the stored rev; in-flight rows are never rewritten), 11.7 (batch life cycle, per-result handling, dependent rows), 11.8 (held and `not_synced` state machine, edits while held queue nothing, Keep uses the tombstone rev, ask again if it changed). 20 new client cases. |
| 6 | Pagination and the high-water mark | **Partly confirmed.** The sequence rule already prevented gaps, but a single-snapshot requirement, the last-page `next_cursor`, per-page atomic apply and a regressed-server signal were missing. | 5.1/5.2, `409 cursor_ahead`, OpenAPI descriptions, two scenarios. |
| 7 | `markReminderSent` scope | **Confirmed unstated.** | 3.2, 11.1, a `local_mutation` case and the M1 exit criteria say it is local-only. |
| 8 | Resurrection rules | **Gaps found**: only `changed` groups were implicitly validated; `created_at` was unprotected; check order and rev monotonicity were implicit. | 7.3: full-task validation, tombstone's `created_at`, `invalid_base_rev` checks first, revs never decrease; 3.3 now requires `task.id` = `task_id`. Scenario `resurrect-validation-and-revs`. |

### Revision 2 validation

All run with throwaway scripts outside the repository (stdlib and the PyYAML already present); no jsonschema or OpenAPI validator exists on this machine.

- Contract structure: OpenAPI 3.1 parses, 134 `$ref`s resolve, 17 operations with unique ids, 44 schemas, path parameters declared, no provider names, LF and no BOM in `api/`. **PASS.**
- Server scenarios: **32 of 32** pass in the reference model (24 original, 8 new: `ambiguous-partial-batch-replay`, `ambiguous-replay-keeps-newer-change`, `reminted-update-would-overwrite`, `replay-returns-stored-rejection`, `restore-payload-validation`, `resurrect-validation-and-revs`, `pull-superseded-between-pages`, `pull-cursor-rules`). The reference model needed one correction (a conflict record lacked the attempted delete's reason): a model gap, not a rule gap.
- Client cases: **49 of 49** (20 original, of which two were rewritten because the rules changed: the in-flight undo and the re-baseline case; 29 new).
- Fixtures against schema: 131 request operations (11 deliberately schema-invalid and rejected by server rules) and 97 responses validated against the `Op`, `PushResponse` and `PullResponse` schemas. **PASS.**
- Caveat as before: one author wrote the rules, the fixtures and the model. Agreement is necessary, not sufficient.

### New or remaining risks after revision 2

| # | Severity | Risk |
|---|---|---|
| N1 | High | **M1 schema grows**: full base snapshots, `held`/`not_synced`, row states with `sent_payload`, `authored_rev`, `collided`, `restore_of`, and a local conflict table. M1 therefore touches more of the live database than first stated. Mitigation: back up `out/bin/data/` outside the repo first, additive migration, test on copies. The digest-versus-copy question is closed: full snapshots (decision 11, revision 3). |
| N2 | Medium | Reconciliation keeps the **cloud value** when both sides changed. A user could prefer their device's value; it is preserved with a restore action, but the UI for many local conflict records (first link after a long offline period) needs design (M5). |
| N3 | Medium | The 60-day rule and `queued_at` use the local clock. A wrong clock only affects efficiency (reconciliation is safe), but is untested with real clock jumps. |
| N4 | Medium | Crash points inside the client's local transactions (marking rows in flight, applying a page and its cursor, applying a push response) are specified (SYNC 11.11) and have fixtures with simulated faults (revision 3), but a pure fixture cannot kill a real SQLite process; M1/M4 need fault-injection tests that really interrupt a transaction. |
| N5 | Medium | The server must really implement single-snapshot pulls (`REPEATABLE READ` or one statement) and the per-account seq lock; only M3 concurrency tests can show it. |
| N6 | Low | A `409 cursor_ahead` and tombstone `created_at` retention are new wire/storage details; both are additive within the unreleased `/v1` draft. |
| N7 | Low | Existing open risks R1 to R13 are unchanged (R1, the unverified Qt 6.8 minimum, and R2, live-database safety, stay High). |

### GO / NO-GO for M1

**GO, conditional on the owner's explicit approval, which has not been given.** The eight issues are specified, covered by fixtures that pass, and none requires a change to the product principles. Conditions to attach to an M1 approval: (1) back up `out/bin/data/` outside the OneDrive checkout and work only on copies; (2) M1 scope is exactly the schema and outbox work in the roadmap row above, no network code; (3) bases are full snapshots (closed in revision 3, nothing left to decide); (4) `markReminderSent` and pull-applied changes write no outbox rows; (5) the client fixtures that do not need a server (`outbox_*`, `local_edit`, `apply_pull`, `apply_result`, `apply_response*`, `reconcile_apply`, `held_transition`, `local_mutation`, `conflict_*`) are wired into Qt Test as part of M1's exit criteria. **NO-GO for M2 onward** until each is separately approved. Nothing proceeds until the owner says so.

## 14. M0 revision 3: three remaining issues

The owner asked for three more issues to be resolved before M1 could be considered. Documentation and fixtures only: no application source, schema, dependency or build file changed. Full bases (not digests) were required.

| # | Issue | Verdict | Resolution |
|---|---|---|---|
| 1 | When does a base snapshot advance? | **Confirmed gap.** SYNC said "when the device adopts a server copy" but not what that excludes, and the dependent-row rule let a queued edit's `base_rev` jump past a change another device made to the same group. | SYNC 11.10: the base is a full snapshot written only from server-supplied copies (invariants B1 to B3); an exhaustive event table (local edits, queued, folded and in-flight rows, timeouts, acknowledgements, replays, rejections, tombstones, unlink, epoch change); walk-throughs for several queued updates and crashes. |
| 2 | Local conflict storage | **Confirmed gap.** Records were mentioned (9.2, 11.6) but had no schema, restore guard, retention or cleanup. | SYNC 11.12: complete logical table, full group values for both sides plus the base value and the cloud revision they belong to, uniqueness, same-transaction preservation, keep, restore and delete, guards G1 to G3, strict restore rows, open records never auto-deleted. |
| 3 | Atomic acknowledgement | **Confirmed gap.** 11.7 applied results "in order" without saying in what transaction. | SYNC 11.11: one `BEGIN IMMEDIATE` transaction per `200` response covering results, revisions, bases, flags, conflict records and row deletion; pre-checks (shape, epoch, link generation); a crash table for every point before, inside and after the transaction. |

**Defect found on the way (caused by issue 1).** Under the old 11.5 and 11.7 wording a pending or dependent `update` was rebased by setting its `base_rev` to the new server revision. If another device had changed a group that edit also changes, the server then saw no collision: the other device's value was overwritten with **no conflict record**, contradicting 6.2 and the "no silent overwrite" requirement of issue 2. SYNC 11.10 now moves `base_rev` forward only when none of the edit's groups collides (otherwise the edit keeps its `base_rev`, or its `authored_rev` if it waited behind an in-flight row), so the server applies 6.2 and records the overwritten value. Restore rows are stricter (cancelled, record reopened). Existing fixtures that disagreed with the new invariants were corrected rather than the rule bent: two `apply_pull` cases held a local value no operation carried (violating B3), and one `write:` case name contradicted its own expectation (renamed). All six edited existing cases are listed in the review package.

### Revision 3 validation

Throwaway scripts outside the repository (standard library and PyYAML). The revision 2 scripts had not been kept, so the reference model was rebuilt from SYNC.md and first checked against the **unchanged** revision 2 fixtures before anything was edited: 32 of 32 server scenarios, 49 of 49 client cases, and the same structural numbers as revision 2 (134 `$ref`s, 131 request operations of which 11 are schema-invalid on purpose, 97 responses). After the edits:

- Server scenarios: **32 of 32** (no server rule changed).
- Client cases: **91 of 91** (49 existing, six of them edited as above, and 42 new: base advancement, atomic acknowledgement and crashes, conflict storage and resolution).
- Mutation check: ten deliberate rule violations in the model (base taken from the local row, collision check removed, non-atomic acknowledgement, a commit despite a fault, restore on the wrong revision, restore without its guard, cleanup of open records, no de-duplication, a local edit moving the base, a non-strict restore row); **10 of 10** make at least one case fail. This shows the new fixtures test the rules, not merely that they agree with the model.
- Structure and schema checks unchanged and passing; `api/` files are LF without BOM.

### New or remaining risks after revision 3

| # | Severity | Risk |
|---|---|---|
| N8 | Medium | A restore row that is already `in_flight` cannot be cancelled; another device's change can still be overwritten by arrival. It is recorded on the server and delivered, never silent (SYNC 11.12). Closing it fully needs a protocol field (an "only if unchanged" guard on `update`), a contract change that was not made. |
| N9 | Medium | Open local conflict records are never deleted automatically, so a device that reconciles many tasks after a long offline period accumulates them until the user acts. The bulk review UI is an M5 item. |
| N10 | Low | `authored_rev` and `collided` add two outbox columns that M1 must migrate. |
| N11 | Medium | Fixtures simulate faults; they do not interrupt a real SQLite transaction (see N4). |
| N12 | Low | Same-author caveat as before: rules, fixtures and model share an author. The mutation check lowers but does not remove it. |

### GO / NO-GO for M1

Unchanged in substance from section 13: **GO, conditional on the owner's explicit approval, which has not been given**, with condition 3 now closed. **NO-GO for M2 onward** until each is separately approved. Nothing has started.

## 15. M1.3 review: deferred items

Review of the remaining M1 findings (F9, F10, F12). Fixed items are covered by tests in `tests/tst_sync.cpp`; these were **deliberately deferred**, because each needs network, backup or UI orchestration that belongs to a later milestone:

| Item | Why deferred | Safe meanwhile |
|---|---|---|
| Epoch-change procedure (SYNC 10: backup, drop outbox, cursor 0, distrust every base) | Needs the byte-identical backup and the user flow of 9.2 (M2+). M1 provides the pieces: `sync_base.trusted`, `applyPullPage(..., serverEpoch)` and `applyPushResponse` both refuse a page or answer from another epoch and apply nothing. | A distrusted base is never used as a `base_rev` (`recordEdit`); the edit stays local and the task is flagged for 9.2. |
| "Unlinking clears holds and `not_synced`" (SYNC 11.8) | Clearing a hold without the relink pass of 9.2 would let a later tombstone delete content the user has not decided about. | `setLinked(false)` leaves holds and marks; test `heldTaskSurvivesUnlinkAndRelink`. |
| Delete of a task whose base is distrusted | The delete cannot choose a trustworthy `base_rev` until 9.2 has pulled the current server state. | Sent with the stored revision; one `invalid_base_rev` retry, then the task is flagged and the next pull decides. |
| `AckFault` test hooks in `SyncStore` | The conformance fixtures drive these points against the same compiled library. | The default `AckFault` takes no fault branch; the 2-argument `applyPushResponse` is the only form production code may call. `AckFault` and the 3-argument overload are private to `SyncStore` and reachable only by the `TestSync` friend; the epoch-less 3-argument `applyPullPage` is private likewise (production must pass `serverEpoch`). |
| Callers of `cleanupConflicts` / `cleanupTombstones` | The sync driver (M2+) decides when; never on a timer. | Both are idempotent and skip anything a queued row still refers to. |

## 16. M2 plan record (server skeleton; approved decisions, no implementation yet)

Status: only **M2.0 (these documentation edits)** is approved. A `server/` directory already exists in the working tree (untracked; this documentation step does not touch it). No PostgreSQL or Docker installation is approved or assumed. M1 and SQLite schema v4 are untouched. M2 contains no sync endpoints and no sync tables (decision 12).

### 16.1 Sub-steps and gates

| Step | Content | Gate |
|---|---|---|
| M2.0 | These documentation edits (`CLOUD.md`, `CLAUDE.md`) | Owner reviews the diff |
| M2.0b | Exact `api/` documentation diff (proposed, **not applied**): password schema, `device_limit_reached`, refresh lifetime, device wording, retention disclosure | Owner approves the diff before `api/` is edited |
| M2.1 | `server/` layout, config, DB session, health, Alembic baseline (account tables of 16.6 only), `.gitignore` Python entries (R11) | Baseline migration applies to the dev database |
| M2.2 | Registration, normalization, Argon2id, password policy, blocklist (source, licence, update procedure, tests), rate limits, ten recovery codes | Policy, normalization and blocklist tests |
| M2.3 | Login, access and refresh tokens, strict rotation (design A), reuse detection, 90 / 180-day lifetime, access tokens bound to the family, logout | Injected-clock tests, including: access token refused the moment its family expires or is revoked |
| M2.4 | Recovery login and code regeneration, change password | Single-use codes; every session revoked |
| M2.5 | Device list and revoke, 25-device cap | Cap refusal creates no session and revokes nothing; revoke frees a slot; cross-account access refused |
| M2.6 | Dockerfile (file only, not built), server README | Files only |

There is no retention purge in M2 (it moved to M3). Server tooling writes under `out/server/`. Each later step needs its own approval, and so do the installations in 16.3.

### 16.2 Retention (decisions 12 and 13)

- Policy: processed operations and conflict records are kept 90 days.
- M2: this policy and its design are documented. No table, no migration and no purge code exist.
- M3: migration 0002 creates the tables; the purge and its automated database tests (boundary at 90 days, referenced rows kept, idempotent, injected clock; no timer) are implemented there.
- Disclosure: deleted task content can remain in conflict history for up to 90 days (SYNC 6.3 wording proposed in M2.0b).

### 16.3 Environment (approved in principle, nothing installed)

Portable Windows PostgreSQL (no service), program and data directory outside OneDrive, localhost only, separate roles and the separate databases `tasknest_dev` and `tasknest_test` (tests refuse any other name). Python virtual environment under `out/server/.venv`. Python packages, the blocklist file and any OpenAPI validator are listed with exact versions and approved separately. Docker is not installed and not required.

### 16.4 Decision: interrupted refresh rotation (design A approved by the owner; B rejected, not implemented)

**The problem.** The server stores refresh tokens only as hashes, so it cannot re-send the raw token of a rotation it already performed. If the response is lost (crash, dropped connection) the client still holds the old token, and presenting it is indistinguishable from an attacker replaying a stolen one.

**Design A: strict rotation, re-login.** Presenting an already-rotated token in a different request than the one that rotated it revokes the whole family; a retry of that exact request (same `operation_id`) is answered `409 refresh_superseded` and revokes nothing (16.7). Either way the client cannot continue with that family and the user signs in again, which creates a new device row (a revoked family stops counting toward the cap; an unrevoked superseded one still counts until revoked or expired, see 16.10). Local data is untouched.

**Design B: encrypted short-lived successor recovery.** On rotation the server also stores the new raw token **encrypted** (authenticated encryption, key from the environment with a key id) on the old token's row, together with a short expiry (proposed 60 s), a use counter and the successor's id. A retry of the old token inside the window returns the identical successor (decrypted), only while the successor has not yet been presented, at most a small number of times (proposed 3), and the request is rate-limited and logged. Outside the window, after the successor has been used, or after the count is exhausted, the old token is treated as reuse and the family is revoked. The ciphertext is erased when the window ends, when the successor is first used, and by the purge. A stateless variant (derive the successor from a server secret, family id and generation) is rejected: a database leak plus that secret would let an attacker mint every future token.

| Aspect | A: strict | B: encrypted recovery |
|---|---|---|
| Security | Strongest. Any use of an old token is detected at once. | Weaker for the window: a thief who replays the old token first receives a valid successor, and the owner's later retry gets the same one, so the theft goes undetected within the window. Raw tokens exist (encrypted) at rest briefly. A database leak alone reveals nothing; a leak plus the key reveals live successors only for the window. |
| Storage | Nothing extra. | Extra columns on `refresh_tokens` (ciphertext, key id, expiry, counter, successor id) and purge of expired ciphertext; one more server secret (key) with rotation by key id. |
| Implementation | Smallest: rotation, reuse detection. | Key management, AEAD, window and counter logic, extra race handling (two concurrent retries, retry racing the first use), more tests. |
| User impact | Rare forced re-login after a lost refresh response; on mobile networks this may be more common than a crash. Local data unaffected; new device row per re-login. | Re-login almost never needed after a lost response. |
| Client | Must persist the new token before using it; no retry logic beyond that. | Same, plus may safely retry once on timeout. |
| Reversibility | Can be upgraded to B later without a contract change. | Weakens detection; going back to A is a server-only change. |

**Decision: A.** B is kept above only as the recorded alternative; no encrypted successor tokens are stored.

### 16.5 Proposed API documentation changes (OpenAPI items in working tree, not approved)

Shown separately for review. The `api/openapi.yaml` items are present in the working tree, uncommitted and awaiting owner approval of the diff; `api/SYNC.md` is not modified, so the SYNC item below is still only proposed: `NewPassword` schema (15 to 128 code points after NFC) used by register, recover and change-password; login password only normalized; error code and `409 DeviceLimit` response for login; refresh lifetime wording (90 sliding, 180 absolute); device wording (active devices, revoke frees a slot, revoking the calling device equals sign-out); SYNC 6.3 retention disclosure with the M3 purge note. The existing `GET /v1/devices` and `DELETE /v1/devices/{id}` operations already exist in the contract.


### 16.6 Access tokens, refresh-token families and devices (data model and checks)

**Tables (M2 baseline, PostgreSQL; all ids UUIDs generated in application code).**

| Table | Key columns | Notes |
|---|---|---|
| `users` | `id` pk, login (normalized, unique), password hash | |
| `devices` | `id` pk, `account_id` fk, name, platform, client_version, `created_at`, `last_seen_at` | One row per sign-in (every login creates one). |
| `refresh_families` | `id` pk, `account_id` fk, `device_id` fk **unique** (one family per device), `created_at`, `absolute_expires_at` (= `created_at` + 180 days), `sliding_expires_at` (starts at +90 days, moved on each rotation but never past `absolute_expires_at`), `revoked_at` null, `revoked_reason` null; unique `(id, account_id)` | The family is the single source of truth for whether a device session is alive. |
| `refresh_tokens` | `id` pk, `family_id` fk, `generation`, `token_hash` unique (SHA-256 of the 256-bit value), `created_at`, `rotated_at` null, `rotation_operation_id` null (UUID of the refresh request that rotated this token; set together with `rotated_at`); **unique `(family_id, rotation_operation_id)`**; index `(family_id, rotated_at)` partial `WHERE rotated_at IS NOT NULL` (serves the rotation-limit count, 16.7) | Only the latest generation (`rotated_at` null) may be rotated. A row with `rotated_at` set that is presented again is a rotated token (16.7 cases 2 and 3). |
| `access_tokens` | `id` pk, `token_hash` unique, `family_id` not null, `account_id` not null, `issued_at`, `expires_at`; **composite fk `(family_id, account_id)` references `refresh_families(id, account_id)`** | Opaque value stored only as a hash. `expires_at` is the earliest of issue time + 15 minutes, `sliding_expires_at` and `absolute_expires_at`. Several rows of one family can authorize at the same time (16.7). |

An access token therefore has exactly one family, and through it exactly one device and one account. The composite foreign key makes an access token whose account differs from its family's account impossible to store.

**Check on every authenticated request (one query, no cache).** The server never takes an account, device or family id from the request to decide who is calling; they come only from the token's row.

```sql
SELECT a.account_id, f.device_id, f.id AS family_id
FROM access_tokens a
JOIN refresh_families f ON f.id = a.family_id AND f.account_id = a.account_id
WHERE a.token_hash = :sha256_of_presented_token
  AND a.expires_at          > :now
  AND f.revoked_at          IS NULL
  AND f.sliding_expires_at  > :now
  AND f.absolute_expires_at > :now;
```

Zero rows (unknown token, expired token, revoked family, expired family) is `401 unauthorized`, with no distinction in the answer. One row gives the caller's account, device and family. Path ids such as `{deviceId}` are then looked up scoped to that account (another account's id behaves like an unknown one). `last_seen_at` is updated at most once a minute. The clock is injectable for tests.

**Connection use by the authentication dependency.** The access-token authentication dependency runs the query above on a connection checked out only for that query and returns it to the pool before the handler body starts. Handlers that hash (password change, recovery-code regeneration, account deletion) therefore hold no connection from the dependency while waiting for an Argon2id permit or hashing (16.11); they check out a new connection afterwards for their own reads and transactions. Validation is not weakened: the token and family check above still runs on every request, with no cache and no reuse of an earlier result, and a request already authorized when its family is revoked may finish (16.7).

**Lifecycle rules.**
- **Login:** two phases; only the second is a transaction that holds a lock. Phase 1, outside any lock or open transaction: (1) read the user's password hash with a plain read and release the connection; (2) acquire an Argon2id permit (16.11) and verify the submitted password against that hash, holding no connection and no row lock; (3) if it is wrong, answer `401` and stop, disclosing nothing about devices. Phase 2, one transaction: (4) begin, lock the user's row (`SELECT ... FOR UPDATE`) and **re-read the hash under that lock**; if it differs from the verified one, apply the stale-hash retry of 16.11 (release this transaction and lock first; if the re-verification then fails the answer is `401`, with no further retry and no mutation; a missing user row at the lock is a generic `401`, 16.11); (5) count active devices (below) and, at 25, apply the replacement rules of 16.8 (`409 device_limit_reached` with nothing written, or an atomic replacement); (6) insert device, family, first refresh token and one access token, then commit. An unknown login name takes the same `401` path after a dummy hash verification of equal cost (dummy hash generated at startup, 16.11).
- **Password change:** two phases. Phase 1, outside any lock or open transaction: read the stored hash, verify `current_password` and hash the new password under Argon2id permits (16.11; no connection or lock held; wrong password: `401`). Phase 2, one transaction: lock the `users` row (`SELECT ... FOR UPDATE`) first, re-read the hash and compare it with the one verified (differs: the stale-hash retry of 16.11, which releases this transaction and lock first and reuses the already-computed new-password hash, re-verifying only the current password under a fresh permit; if that re-verification fails the answer is `401` with no further retry and no mutation; a user row that no longer exists at the lock answers a generic `401` with no retry and no mutation), store the new hash, revoke all other families of the account, commit.
- **Why this serializes login against a password change.** The second-phase transactions of both begin by taking `SELECT ... FOR UPDATE` on the same `users` row, so those transactions run one after the other, never interleaved. The hash is verified outside the lock, but a session is created only if the hash re-read under the lock is the one that was verified. If a password change committed first, the login finds a different hash under the lock and takes the one-retry rule (16.11), which verifies against the new hash, so a submission of the old password fails with `401`. If the login took the lock first, the change waits until the login has committed its session, and then revokes every other family, including the one just created. A session therefore cannot be created from a password verified against a superseded hash, because the hash is compared under the lock before anything is written. (Without the re-read, login could verify H1, a change commit H2 and revoke, and the login then insert a session the change never saw.) No row lock or connection is held during hashing, so concurrent attempts on one login name do not queue behind a slow hash; a stale hash costs at most one extra verification.
- **Argon2id verification and lock ordering.** Every password check is an Argon2id verification (parameters of 16.11) of the NFC-normalized submission against the encoded hash read from the `users` row; an unknown login name runs a dummy Argon2id verification intended to cost the same. The concurrency semaphore that limits simultaneous hashes (section 7, Passwords) is a **leaf**: a permit is held only while hashing, never while a database connection or row lock is held, so a semaphore holder never waits for a lock or a connection and no cycle with them is possible. Row-lock order, everywhere, is: (1) the `users` row, then (2) `refresh_families` rows, several of them in ascending `id`; no other row is locked, and the `users` lock is never requested while a family lock is held. Login, password change, recovery, recovery-code regeneration and account deletion take `users` first (recovery and account deletion then lock all refresh families of the account in ascending `id`; regeneration locks no family); a replacement locks the target family after `users` (16.8); register acquires no `users` row lock, and refresh, logout and device revoke take only family rows. Hashing (including the new password in a password change or recovery) happens before the `users` lock is taken, never under it. Because every path acquires locks in this one order, no cycle is possible. The Argon2id concurrency decisions are recorded in 16.11.
- **Refresh:** the five-case precedence, the family-row lock and the rotation limit of 16.7. A successful refresh marks the presented token rotated, inserts the next generation, moves `sliding_expires_at` (capped at `absolute_expires_at`) and issues one new access token. **Earlier access tokens of the family are neither deleted nor shortened**; each stays valid until its own `expires_at` or until the family stops being alive.
- **Logout, device revoke, recovery, password change:** set `revoked_at` on the affected families (recovery revokes all families of the account; a password change revokes all other families). Access tokens need no separate deletion because the query above already refuses them.
- **Expiry needs no job:** the check compares timestamps to `now`, so a family stops authorizing the instant either expiry passes, whether or not any cleanup has run. Expired `access_tokens` rows are deleted opportunistically; that never matters for correctness.
- **Device slots:** a device is *active* iff its family has `revoked_at IS NULL AND sliding_expires_at > now AND absolute_expires_at > now`. This is the same predicate as the request check, so a device that no longer counts against the cap also cannot authorize anything. `GET /devices` lists only active devices.

**Tests (server, injected clock, dedicated test database; M2.3 and M2.5).**

| Case | Expected |
|---|---|
| **Expired family, sliding** | Advance the clock past `sliding_expires_at` while the access token's own 15 minutes have not passed (possible only if the token was not already clamped, so the test also writes such a row directly): the access token is refused (`401`), refresh is `401`, the device is absent from the list and does not count toward the cap. |
| **Expired family, absolute** | Keep refreshing so sliding keeps moving; at `created_at` + 180 days the access token and refresh are both `401`; sliding never extends past the absolute limit; a new login succeeds. |
| **Access token expiry clamp** | A token issued 5 minutes before the family's expiry has `expires_at` equal to the family expiry, not +15 minutes. |
| **Revoked family** | After logout, device revoke, reuse detection, recovery and password change, the family's access token is refused immediately (before its own expiry); other families and accounts are unaffected; the revoked device frees a slot. |
| **Old access token after a subsequent login** | Device A signs in, then device B signs in (a new device and family). A's token still authorizes as A only (`current` is A in `GET /devices`) and B's as B only; neither ever authenticates as the other. After A refreshes, A's previous access token still works as A until its own expiry, and so does the new one; neither ever authenticates as B. Recovery after both logins refuses both. |
| **Access token of a different family** | A's token cannot act as B: `DELETE /devices/{B}` with a token of another account gives `404`; a token row cannot be stored with a family of another account (composite fk violation); presenting B's refresh token with A's access token has no effect on A's session; A's token is not accepted after A's family is revoked even though B's family is alive. |
| **Cap and races** | 25 active devices admit no 26th (`409`, no session created, nothing revoked); revoking one lets a login succeed; an expired or revoked device does not count; two concurrent logins at 24 devices yield exactly one success. |
| **Strict rotation** | Using a rotated refresh token in a different request (case 3 of 16.7) revokes the family and every access token of the family stops working at once; no grace window. |
| **Operation-ID precedence** | One test per case 1 to 5 of 16.7, including: case 2 revokes nothing and the latest token still refreshes; case 4 changes no row; a rotated token is never evaluated as case 4; revoked or expired family with a matching operation id is case 1. |
| **Same operation ID retry** | A refresh rotates the token; a second request with the same token and the same `operation_id` (the lost-response retry, also when concurrent) yields exactly one successful rotation and `409 refresh_superseded` for the other; the family is not revoked, its latest token still refreshes with a new `operation_id`, and no state changes. |
| **Different operation IDs, same rotated token** | A token is rotated, then presented again with a different `operation_id`: case 3 of 16.7, `401` and the family is revoked at once (reason `reuse`), every access token of the family stops authorizing. When two concurrent requests carry different operation ids for the same token, one rotates and the other follows case 3. |
| **Rotation limit** | Six rotations in 15 minutes succeed (seven authorizing access tokens with the login token), the seventh attempt is `429` with `Retry-After` (whole seconds, rounded up, at least 1) and no state change; a rotation exactly 15 minutes old (`rotated_at = now - 900`) is outside the window, one second younger is inside; two concurrent refreshes at five rotations yield exactly one success. |
| **Replacement** | The rules and rollback test of 16.8: the 409 `details` is exactly `{limit: 25, active: 25}` (integers, no `devices` array, no other key); byte-identical 409 for a missing, unknown, inactive and foreign `replace_device_id`; a wrong password with any `replace_device_id` is `401` and no device information; password verification precedes device inspection; a target family revoked or expired by a concurrent transaction before the family lock is granted yields the identical 409; a forced failure while creating the new session leaves the old device active. |
| **Address limit and proxies** | The rules of 16.9, including the original fixtures T1 to T10 exactly as written there (fixture list only; production `trusted_proxies = []`), and additional normalization tests T11 onward (IPv4-mapped IPv6, IPv6 `/64` aggregation, zone identifier). Untrusted peer cannot spoof; invalid or disagreeing forwarding headers fall back to the socket peer. **Conditional:** the 121st-admitted-request test (121st refused without counting itself) exists only if the owner approves **both** the endpoint scope (A1) and the storage (A7); until then it is not an M2 test. |

### 16.7 Refresh protocol (D2)

**Tokens.** Refresh and access tokens are opaque values with 256 bits of entropy from a CSPRNG; only SHA-256 hashes are stored. A rotation returns the raw successor once. It is never stored or encrypted (design B stays rejected), so it cannot be recovered. A refresh family is one device. Lifetime: 90 days sliding, 180 days absolute from family creation (decision 15).

**Request.** `POST /v1/auth/refresh` carries `refresh_token` and `operation_id`, a client-generated UUID. The client reuses an `operation_id` only to retry that exact request (same token, after a timeout or lost response) and uses a fresh one for every new refresh.

**Serialization.** The server finds the presented token by hash, then locks that token's `refresh_families` row (`SELECT ... FOR UPDATE`), then re-reads the token row and the family under the lock and decides. Refreshes of one family are therefore strictly ordered, and the rotation count and the rotation are one atomic step. Lock order: exactly five operations take the `users` row first and then `refresh_families` row(s): login (at most one family row, 16.8), password change (all other families of the account), recovery (all families of the account), recovery-code regeneration (no family rows) and account deletion (all families of the account). Register, refresh, logout and device revoke do not acquire the `users` row lock. Wherever several family rows are locked they are locked in ascending `id` order. Refresh-specific rule: refresh locks only its own family row (that of the presented token), and no operation that holds a family row requests a `users` row.

**Precedence (evaluated top to bottom; the first match decides):**

| # | Condition | Result | State change |
|---|---|---|---|
| 1 | Token unknown, or its family revoked or past either expiry | `401 unauthorized` | none |
| 2 | Token already rotated and its `rotation_operation_id` equals the request's `operation_id` | `409 refresh_superseded` | none; the family is **not** revoked |
| 3 | Token already rotated with a different `rotation_operation_id` | `401 unauthorized` | family revoked (`revoked_reason` = `reuse`) |
| 4 | Token is the latest generation, but `operation_id` is already the `rotation_operation_id` of another token of the same family | `401 unauthorized` | none |
| 5 | Token is the latest generation and `operation_id` is unused in the family | rotation limit check; then rotate (`200`) or `429` | rotation: see below; `429`: none |

Cases 3 and 4 are different: in 3 a token that was already spent is replayed (possible theft, so the family is revoked); in 4 the current token is presented with an operation id that belongs to an earlier rotation (a client defect or a mismatched replay), which is refused without punishing the family. The database enforces uniqueness of `(family_id, rotation_operation_id)`, so a concurrent duplicate cannot rotate twice even if the lock were bypassed; a violation is treated as case 4 (`401`, rolled back).

**Case 2 consequence.** The client's earlier request did rotate the token, but its response was lost, and the raw successor cannot be recovered. The client must discard the stored token and sign in again. The superseded family stays alive and counts toward the 25-device cap until it is revoked or expires (16.10). The client may use `replace_device_id` (16.8) to replace it.

**Rotation limit.** At most **six successful rotations per family in any rolling 15-minute window**. Under the lock: `n = count(refresh_tokens WHERE family_id = :f AND rotated_at > :now - interval '15 minutes')`. A rotation exactly 15 minutes old is outside the window (strict `>`). If `n >= 6`, answer `429 rate_limited` with `Retry-After`, change nothing. Otherwise rotate in the same transaction: set `rotated_at` and `rotation_operation_id` on the presented token, insert the next generation, move `sliding_expires_at` (never past `absolute_expires_at`), insert one access token (`expires_at` = earliest of now + 15 minutes, sliding, absolute). `Retry-After` = whole seconds, rounded up, at least 1, until the oldest counted rotation becomes 15 minutes old: `max(1, ceil(oldest_rotated_at + 15 min - now))`. The count uses the partial index `(family_id, rotated_at) WHERE rotated_at IS NOT NULL`.

**Coexisting access tokens.** A refresh does not delete or shorten earlier access tokens of the family. An access token *authorizes* a request only while both hold: its own `expires_at > now`, and its family is not revoked and not past either expiry (query in 16.6). A row that is unexpired by its own `expires_at` but belongs to a revoked or expired family exists in the table and authorizes nothing, so "unexpired" alone is not the bound. Because an access token lives 15 minutes and at most six rotations fall in any rolling 15-minute window, one family has at most **seven authorizing access tokens** (the login token plus six). Only active families can authorize, and an account has at most 25 of them, so an account has at most **175 authorizing access tokens** (7 x 25). The 175 bounds tokens that can authorize, not stored rows: rows of revoked or expired families and expired rows not yet deleted may exist in addition. Every authorizing token is checked against its family on each request, so revoking or expiring the family stops all of them at their next authorization check. A request already authorized when the family is revoked may finish.

### 16.8 Login sequence (every login) and device replacement at the cap

Steps 1 to 3 below (verify outside locks, lock `users`, re-read the hash under the lock) and the active-device count of step 4 run on **every** login, whether or not the account is at the device limit. Only the family lock, the replacement and the `409` of steps 4 to 7 depend on reaching 25 active devices.

- **Request.** `POST /v1/auth/login` accepts an optional `replace_device_id` (UUID of one of the caller's own active devices, for example read from `GET /devices` on a device that is still signed in). It is meaningful only at the limit.
- **Below the limit** (fewer than 25 active devices): `replace_device_id` is ignored, nothing is revoked, no error is raised for any value that is well-formed.
- **Order (steps 1 to 3 and the count in step 4 for every login; the rest only at the limit):**
  1. Outside any lock or transaction: read the user's password hash (an unknown login name uses the startup dummy hash), acquire an Argon2id permit (16.11) and verify the password off the database, holding no connection and no row lock. Wrong: `401`, nothing about devices is disclosed, stop. Password verification therefore always precedes any device inspection.
  2. Begin the transaction and lock the user's row (`SELECT ... FOR UPDATE`).
  3. Re-read the hash under that lock and compare it with the verified one. If it differs, apply the stale-hash retry of 16.11: roll back and release the lock, acquire a fresh permit, verify again outside locks, then repeat from step 2. If that re-verification fails, the answer is `401`, with no further retry and nothing written; a second hash mismatch under the lock answers `503 server_busy`, nothing written. A missing user row at the lock answers a generic `401`, no retry, no mutation.
  4. Count active devices; at 25 and `replace_device_id` present, lock the target family row (`SELECT ... FOR UPDATE` on the `refresh_families` row whose device is `replace_device_id` **and** whose `account_id` is the caller's, so a foreign id is never locked). No row: go to step 6.
  5. Recheck, while holding that lock, that the family belongs to the caller's account and is active (`revoked_at IS NULL AND sliding_expires_at > now AND absolute_expires_at > now`). A state read before the lock is not trusted.
  6. If the target is missing, unknown, inactive or foreign, or `replace_device_id` is absent: roll back and answer the identical `409 device_limit_reached`. Nothing was written.
  7. Otherwise, in the same transaction, revoke the family (`revoked_reason` = `replaced`) and create the new device, family, first refresh token and access token. Any failure rolls everything back and the old device stays active.
  8. Commit.
  Lock order is `users` row, then one family row (16.6). No permit is acquired or held while a lock or connection is held.
- **409 body.** `details` contains **exactly** `limit` and `active`, both integers fixed at 25 (`{limit: 25, active: 25}`). There is no `devices` array and no other key. The status, code, message and `details` are identical whether `replace_device_id` is absent, unknown, inactive (revoked or expired) or belongs to another account; **404 is never returned for it**, so the id cannot be used to probe other accounts. The response never discloses device information; the client takes device ids from `GET /devices` on a device that is still signed in (see A11).
- **Success.** The normal `AuthResponse` plus `replaced_device_id` (the revoked device's id), present only when a replacement happened.
- **Atomicity.** Steps 7 and 8 make revocation and creation one atomic change. Concurrent logins and password changes are serialized by the `users` lock; a refresh on the replaced family blocked on the family lock then sees it revoked (`401`).
- **Ordering.** The old device's access tokens stop at their next authorization check; requests already authorized may finish.
- **Registration** creates the first device of a new account and has no replacement.

### 16.9 Address rate limit and trusted proxies

**Limit.** 120 admitted requests per client address per rolling 15-minute window. A request is admitted if fewer than 120 admitted requests with the same key lie in the window (strictly newer than now - 15 min). A refused request is answered `429 rate_limited` and is **not recorded**, so refusals never extend the lockout. `Retry-After` = `max(1, ceil(oldest_counted_admitted_at + 15 min - now))` seconds. The check runs before authentication and before any other processing, and a refused request changes no state. Counting and admission must be atomic per key. Where the state lives is **not decided here** and is an open owner decision (A7, "Storage of the address-limit state" below); the contract fixes only the observable behaviour above.

**Proposed scope (NOT approved; owner decision required).** The limit would apply to exactly these four unauthenticated endpoints, individually:

1. `POST /v1/auth/register`
2. `POST /v1/auth/login`
3. `POST /v1/auth/refresh`
4. `POST /v1/auth/recover`

Assumption behind it: these are the endpoints reachable without a token, hence the ones an anonymous client can use for guessing passwords or recovery codes or for mass registration. One shared counter per address across the four is assumed (not one per endpoint). **Not included, and not silently extended:** the authenticated endpoints (`/auth/logout`, `/auth/recovery-codes`, `/auth/password`, devices, sync, export, `/account/delete`), `/health` and `/version`. Those keep their existing per-account limits (section 7, SYNC 12), whose numbers are outside this revision. Until the owner approves the scope, the 429 responses on other operations in `openapi.yaml` keep their existing meaning.

**Key normalization.** IPv4: the dotted quad (`/32`). An IPv4-mapped IPv6 address (`::ffff:a.b.c.d`) is converted to IPv4 first. IPv6: the first 64 bits, i.e. the address masked to `/64`, in canonical lower-case form. Examples: `192.0.2.7` -> `192.0.2.7`; `::ffff:192.0.2.7` -> `192.0.2.7`; `2001:db8:1:2:aaaa::1` and `2001:db8:1:2:bbbb::9` -> `2001:db8:1:2::/64`. Zone identifiers (`%eth0`) are stripped from the socket peer and make a forwarded value invalid.

**Trusted proxies.** `trusted_proxies` is a server setting, a list of addresses or CIDR ranges, **default empty**. Deterministic client-address selection:

1. Take the immediate socket peer (normalized as above). If it is not in `trusted_proxies`, the client address is the peer and **all forwarded headers are ignored** (an untrusted client cannot spoof its key by sending them).
2. If the peer is trusted, parse the headers present. `X-Forwarded-For`: all header lines joined with `,`, split on `,`, each element trimmed, optionally `[v6]` with or without a port or `v4:port`; any element that is not a valid IP makes the header invalid. `Forwarded` (RFC 7239): all lines joined, split on `,` into elements, each element's `for=` parameter read (quoted string allowed; brackets and port stripped); `unknown`, obfuscated (`_x`) or missing `for` values make the header invalid. Elements without `for` that carry only other parameters also make it invalid (assumption: no guessing).
3. For a valid header, walk its address list **from right to left** (nearest proxy first), skipping addresses in `trusted_proxies`; the first address not trusted is that header's client address. If every address is trusted, the leftmost is used.
4. One header present and valid: use its client address. Both present: use it only if both are valid and select the **same normalized address**; if either is invalid or they differ (they "disagree"), ignore both and use the socket peer. Neither present, or the one present is invalid: the socket peer.
5. Normalize the chosen address to the key as above.

Spoofing resistance: forwarded values count only from a trusted peer; a trusted proxy must append the address it saw, so entries a client prepends sit to the left and are never selected while a rightmost untrusted address exists. Requires that the proxy is the only network path to the server and that it overwrites or appends (documented for operators). Assumptions: the two headers are compared by their selected client address, not by the whole chain; NAT64 and 6to4 forms are not unwrapped.

**Fixtures T1 to T10 (contract fixtures for the selection rules above; original approved set, restored unchanged).** For these fixtures only, `trusted_proxies = [10.0.0.0/24]`. That value is a **labelled test fixture**; the production value is `trusted_proxies = []` and stays empty. XFF is `X-Forwarded-For`; Fwd is `Forwarded`. "Peer" is the socket peer.

| ID | Peer | XFF | Fwd | Expected key | Rule shown |
|---|---|---|---|---|---|
| T1 | `203.0.113.9` (untrusted) | `198.51.100.10` | absent | `203.0.113.9` | Untrusted peer: forwarded headers ignored |
| T2 | `10.0.0.2` (trusted) | `198.51.100.10` | absent | `198.51.100.10` | Trusted peer, one valid header |
| T3 | `10.0.0.2` (trusted) | `198.51.100.10, 10.0.0.5` | absent | `198.51.100.10` | Right to left, trusted `10.0.0.5` skipped |
| T4 | `10.0.0.2` (trusted) | absent | `for="[2001:db8:abcd:1::42]"` | `2001:db8:abcd:1::/64` | Quoted bracketed IPv6, `/64` key |
| T5 | `10.0.0.2` (trusted) | `198.51.100.10` | `for=198.51.100.11` | socket peer `10.0.0.2` | Both valid but select different addresses |
| T6 | `10.0.0.2` (trusted) | `198.51.100.10` | `for=198.51.100.10` | `198.51.100.10` | Both present and select the same address |
| T7 | `10.0.0.2` (trusted) | absent | `for=unknown` | socket peer `10.0.0.2` | Invalid `Forwarded` value |
| T8 | `10.0.0.2` (trusted) | `192.0.2.10:443` | absent | `192.0.2.10` | IPv4 port stripped |
| T9 | `10.0.0.2` (trusted) | `[2001:db8::10]:443` | absent | `2001:db8::/64` | Bracketed IPv6 with port, `/64` key |
| T10 | `10.0.0.7` (trusted) | `10.0.0.5, 10.0.0.6` | absent | `10.0.0.5` | Every address trusted: leftmost |

**Additional normalization tests start at T11** (none of them replaces T1 to T10). Proposed, expected results derived from the key rules above, with `trusted_proxies = []` unless stated: T11 peer `::ffff:198.51.100.9` -> `198.51.100.9`; T12 peers `2001:db8:1:2:aaaa::1` and `2001:db8:1:2:bbbb::9` -> both `2001:db8:1:2::/64`; T13 peer `2001:db8:1:3::1` -> `2001:db8:1:3::/64` (different key from T12); T14 peer `fe80::1%eth0` -> `fe80::/64` (zone stripped).

**Storage of the address-limit state (open; nothing chosen).** A rolling-window limit with an exact count needs state that is atomic, shared and durable enough. What each situation requires:

- **Multiple workers (threads or async tasks, one process):** a process-local structure behind a lock is atomic.
- **Multiple processes (several workers, several containers):** process-local state is per process, so a client reaches N times the limit (120 x N) and a sticky-routing change silently changes the effective limit. The state must be shared.
- **Restarts:** process-local state is lost, so a restart resets every counter; a shared store survives a server restart only if it is itself durable.

| Alternative | Multi-worker | Multi-process | Restart | Trade-offs |
|---|---|---|---|---|
| **S1. In-process memory** (per-key deque and lock) | Exact | **Not exact**: limit is multiplied by the process count | Counters reset | No dependency, no table, fastest. Only honest for a single-process deployment; the limit must then be documented as per process. A restart or a crash loop erases the protection. |
| **S2. Existing PostgreSQL with a new table** (one row per admitted request, or a per-key window row; atomic by a per-key advisory lock or `INSERT ... ON CONFLICT`) | Exact | Exact | Survives | Needs a **new table** and a migration, so it changes the M2 table baseline (16.6) and requires explicit approval. Adds a write per unauthenticated request, which a flood turns into database load (the attack surface it is meant to protect), plus periodic purge of old rows. |
| **S3. PostgreSQL without a table** (`pg_advisory_xact_lock(hash(key))` only) | Exact | Exact | **No state**: the lock serializes but nothing is stored | Not sufficient alone; it needs some state to count, so it reduces to S2 or S1. Listed to show why "advisory lock" alone is not an answer. |
| **S4. Shared external store** (for example Redis with a Lua script or sorted set) | Exact | Exact | Survives only if persistence is configured | New dependency and a new service to run, secure and back up; the server would need it for any deployment. **Requires explicit dependency approval**; not assumed. |
| **S5. Reverse proxy or gateway limit** (the operator's proxy counts, the app does not) | n/a | Exact per proxy instance | Proxy-dependent | Keeps the application stateless and dependency-free, but the limit is outside the contract, differs by operator, and cannot give the `Retry-After` and exact-window semantics of this section. The application would only document the requirement. |
| **S6. Approximate fixed-window counter in memory with documented slack** | Exact within a process | Limit multiplied by process count | Resets | A weaker guarantee than the rolling window specified above; would need that wording relaxed by the owner. |

Consequence for the text above: the exact rolling-window rule and "a refused request is not recorded" are guaranteed only by S2 or S4, or by S1 in a single-process deployment. Until the owner picks one, the implementation must not claim the exact guarantee, and the endpoint scope above stays **NOT APPROVED**.

### 16.10 D2 assumptions and open points

| # | Item | Status |
|---|---|---|
| A1 | Endpoint scope of the 120-request address limit | **Proposed, not approved** (16.9) |
| A2 | One shared counter across the four proposed endpoints | Assumption |
| A3 | After `refresh_superseded` the family stays alive and keeps its device slot until revoked or expired; the client re-logs in and may pass `replace_device_id` | Assumption (follows from "no family revocation") |
| A4 | A rotation-limit `429` and an address-limit `429` share the code `rate_limited`; `Retry-After` tells the wait | Assumption |
| A5 | Malformed `replace_device_id` (not a UUID) is a `422` schema error before the password check; this reveals nothing about any account | Assumption |
| A6 | Login-name bounds (3 to 254) are unchanged; only password fields were re-bounded | Assumption |
| A7 | Storage of the address-limit state (below). Nothing is chosen: no table is added to the M2 baseline of 16.6, and no Redis or other dependency is introduced | **Open, owner decision required** |
| A8 | Forwarded-header parsing details in 16.9 (invalid on any unparsable element, comparison by selected address) | Assumption |
| A9 | The existing per-account limits on authenticated endpoints and on recovery attempts per account (section 7) are unchanged by this revision | Assumption |
| A10 | Fixtures T1 to T10 (16.9) are the original approved set; they and T11 onward are expected results of this contract, not recorded behaviour of any implementation. `10.0.0.0/24` is a test fixture only; production `trusted_proxies` is `[]` | Assumption |
| A11 | Because the `409` carries no device list, a client at the cap learns device ids only from `GET /devices` on a device that is still signed in; with none, the user revokes there or uses recovery (which revokes all sessions). Whether a client-side aid is needed is not decided here | Assumption |
| A12 | Argon2id concurrency: capacity, queue, timeout, timeout response, dummy hash | **Resolved** by owner approval of Option B (16.11) |
| A13 | The 121st-admitted-request test is conditional on owner approval of both the endpoint scope (A1) and the storage (A7) | **Open, conditional** |
| A14 | A second stale-hash mismatch under the row lock returns `503 server_busy` (16.11) | **Approved** by the owner |
| A15 | Regenerate recovery codes: stale-hash verification and retry as in 16.11 (failed re-verification: `401`, no further retry, no mutation; missing user row: generic `401`); explicitly locks the `users` row and replaces the recovery codes transactionally | **Approved** by the owner |
| A16 | Delete account: stale-hash verification and retry as in 16.11 (failed re-verification: `401`, no further retry, no mutation; missing user row: generic `401`); explicitly locks the `users` row, then all of the account's refresh families in ascending `id` order, then deletes | **Approved** by the owner |
| A17 | Password change retry: reuse the already-computed new-password hash and re-verify only the current password under a fresh permit. A failed re-verification returns `401`, with no further retry and no mutation. A missing user row returns a generic `401`, with no retry and no mutation | **Approved** by the owner |
| A18 | Cancellation: a waiting request is removed from the queue; successful pre-start cancellation releases the permit; cancellation during hashing retains the permit until the worker finishes. A dedicated executor has at least two threads, while the limiter independently enforces two concurrent hashes and six waiting requests | **Approved** by the owner |
| A19 | Recovery with a code that is found already consumed after the `users` lock (a lost concurrent race, or a code used a moment earlier): the response is the same generic `401` as an unknown or wrong code (already listed for `recoverAccount`; no new status code). An already-consumed code returns the generic `401` with no retry, no mutation, and the transaction is rolled back. Nothing is changed in every case | **Approved** by the owner |
| A20 | Which of the eleven `503 server_busy` operations can return it from the 3-second row-lock or pool timeout (16.11): pool-checkout timeout: all eleven; row-lock-wait timeout: only the eight operations that take `SELECT ... FOR UPDATE` (not `register`, `listDevices`, `exportData`) | **Approved** by the owner |

### 16.11 Argon2id concurrency (approved Option B)

- **Parameters:** Argon2id `m=19456` KiB, `t=2`, `p=1`. One Uvicorn worker.
- **Permits:** two concurrent hashes, six waiting, a 2-second acquisition timeout. A full queue or a timeout answers `503 server_busy` with `Retry-After: 2`; nothing is changed.
- **Dummy hash:** generated at startup with the same parameters; used for unknown login names.
- **Scope:** a permit is acquired only by operations that perform Argon2 work (register, login, recover, regenerate recovery codes, change password, delete account). Refresh, logout and device revoke acquire none.
- **No locks while hashing:** no database connection and no row lock is held during hashing. This includes the connection of the access-token authentication dependency, which is returned to the pool before the handler of password change, regeneration or account deletion starts hashing (16.6); token and family validation itself is unchanged. The password hash is re-read under the `users` row lock and compared with the hash that was verified. On a mismatch the operation retries once (verify again against the new hash); a second mismatch returns `503 server_busy` (A14).
- **Stale-hash retry (exact sequence):** on the first mismatch the operation rolls back its transaction and releases the `users` lock and its connection; it then acquires a **fresh** Argon2id permit with the same 2-second timeout and the same six-waiter queue limit (a full queue or a timeout answers `503 server_busy`, `Retry-After: 2`, nothing changed), verifies again against the newly read hash with no lock or connection held, releases the permit, and begins a new transaction that locks the `users` row and re-reads the hash. If it matches, the operation proceeds. If it differs a second time, the answer is `503 server_busy` and nothing is changed. There is no third attempt.
- **Cancellation (A18, approved):** if a request is cancelled while its worker thread is still hashing, the permit is retained until that thread finishes, so cancelled work cannot exceed the capacity of two. A request cancelled while waiting for a permit is removed from the queue, never holds a permit, and changes nothing. A request cancelled after it holds a permit but before its worker thread has started hashing releases the permit at once (successful pre-start cancellation; no hashing is running). In all cases no lock or connection is held (no permit is ever acquired while holding either), so a cancellation at these points has no database effect.
- **Executor and limiter:** hashing runs on a dedicated executor with **at least two threads**. The limiter is independent of the executor size and itself enforces two concurrent hashes and six waiting requests.
- **Password change retry (A17, approved):** on the stale-hash retry the already-computed new-password hash is reused (not hashed again); only the current password is re-verified, under a fresh permit. If the user row no longer exists when the lock is requested, the answer is a generic `401` with no retry and no mutation.
- **Password change family revocation:** after the `users` row is locked and the hash check passes, the transaction locks the account's refresh families other than the caller's own, in ascending `id` order (`SELECT ... FOR UPDATE ... ORDER BY id`), then sets `revoked_at` on them and stores the new hash, and commits. This follows the single lock order of 16.6 (`users` first, then families); the family rows are locked here only after the `users` row, and a concurrent refresh holding one family row never waits for the `users` row, so no cycle is possible.
- **Missing user row (login, recovery, recovery-code regeneration, account deletion):** if the `users` row no longer exists when its lock is requested (for example the account was deleted during hashing), the answer is a generic `401 unauthorized`, with no retry and no mutation (a recovery code is not consumed), identical to a wrong password. This is the same rule A17 states for password change.
- **Failed verification after the stale-hash retry:** if, after the stale-hash retry (verify again under a fresh permit), the current password or login password does not verify against the newly read hash, the answer is `401 unauthorized`, with no further retry and no mutation. Only a second hash mismatch under the lock (A14) answers `503 server_busy`.
- **Cancellation after a transaction begins:** if a request is cancelled once its transaction has begun (while waiting for a lock, holding locks, or between statements), the transaction is rolled back and its row locks and database connection are released, with no mutation committed. During a stale-hash retry the same permit rules as A18 apply to the newly acquired permit: cancellation before its hashing worker has started releases that permit at once; once hashing has started, the permit is retained until the worker thread finishes.
- **Permit and lock paths per operation** (permits are always taken and released before any `users` lock; hashing never happens under a lock):
  - *Register:* permit, hash, then insert; no stale-hash rule (no existing hash).
  - *Login:* as in 16.6 and 16.8 (verify outside, lock `users`, re-read, stale-hash retry).
  - *Change password:* as in 16.6 (verify current password and hash the new one outside locks; lock `users`, re-read, stale-hash retry per A17: reuse the new hash, re-verify only the current password under a fresh permit; missing user row is a generic `401`, no retry, no mutation).
  - *Recover:* the recovery code is checked by keyed HMAC (no permit); the new password is hashed under a permit before the `users` lock. Then begin a transaction and lock `users` (a missing user row: generic `401`, no retry, no mutation). **After acquiring that lock, re-read the recovery-code record and check that it is still unused**; consume it atomically in this same transaction (a conditional update that succeeds only if the code is still unused, requiring exactly one affected row) before the new hash is stored; then store the hash, lock all families of the account in ascending `id` and revoke them, commit. The HMAC verification stays outside all locks; only the unused-check and the consumption happen under the lock. Every recovery takes `users` first, so concurrent requests with the same code are serialized and at most one can consume it; the others find it consumed, roll back and change nothing. A code found already consumed after the lock answers the same generic `401` as an unknown or wrong code, with the transaction rolled back (A19, approved). There is no stored password hash to verify, so the stale-hash rule does not apply.
  - *Regenerate recovery codes (A15, approved):* the password is verified under a permit outside locks; then begin a transaction, lock the `users` row, re-read the hash and compare (stale-hash retry as above), and replace the recovery codes in that transaction.
  - *Delete account (A16, approved):* the password is verified under a permit outside locks; then begin a transaction, lock the `users` row, re-read the hash and compare (stale-hash retry as above), lock all of the account's refresh families in ascending `id` order, then delete.
  - *Refresh, logout, device revoke:* no permit.
- **Database timeouts:** row-lock wait and pool checkout timeouts are 3 seconds. Pool starting values: 5 connections, 0 overflow, pre-ping.
- **API:** the eleven operations that may return `503 server_busy` are listed in `api/openapi.yaml`; `exportData` only before its first response byte. Health, version and sync operations do not return it. The eleven are `register`, `login`, `refreshToken`, `logout`, `recoverAccount`, `regenerateRecoveryCodes`, `changePassword`, `listDevices`, `revokeDevice`, `exportData` and `deleteAccount`; this scope is unchanged.
- **Causes of `503 server_busy` per operation.**
  - *Argon2 permit (full queue or 2-second timeout):* only the six operations that hash (Scope above): `register`, `login`, `recoverAccount`, `regenerateRecoveryCodes`, `changePassword`, `deleteAccount`. Never `refreshToken`, `logout`, `revokeDevice`, `listDevices`, `exportData`.
  - *Second stale-hash mismatch under the lock (A14):* only the operations with a stale-hash rule: `login`, `changePassword`, `regenerateRecoveryCodes`, `deleteAccount` (the same `503`, `Retry-After: 2`).
  - *3-second database timeouts (A20, decided by the owner):* the pool-checkout timeout can return `503` from all eleven operations, including the authentication dependency's own checkout (16.6) for the authenticated ones. The row-lock-wait timeout can return it only from the operations that take a `SELECT ... FOR UPDATE`: `login`, `changePassword`, `recoverAccount`, `regenerateRecoveryCodes`, `deleteAccount` (`users` row, then families), and `refreshToken`, `logout`, `revokeDevice` (family rows). `register`, `listDevices` and `exportData` take no such lock and return `503` only from the pool timeout.
