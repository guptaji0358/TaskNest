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
| 15 | **Session lifetime.** Refresh tokens slide 90 days and end at 180 days after the token family was created, with rotation and reuse detection. Rotation is **strict (design A, approved)**: no grace window, a lost refresh response means re-login (section 16.4). | Section 7 |
| 16 | **Device cap.** At most 25 active devices per account. A sign-in over the cap is refused with `409 device_limit_reached`; no device is ever revoked automatically. Device list and revoke are part of M2. | Section 7; SYNC (proposed) |

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

- **Passwords:** Argon2id at OWASP-minimum or higher parameters. On a 512 MB free instance, limit concurrent hashes with a semaphore.
  - **Policy (decision 14):** applied to the NFC-normalized password. Minimum 15 and maximum 128 Unicode code points (counted after NFC). Case and whitespace are preserved (nothing trimmed or collapsed). No composition rules and no periodic changes. Passwords are normalized with **NFC**; login identifiers with **NFKC + casefold**. The 15-character minimum follows NIST SP 800-63B-4 for a password used as the only authenticator (recovery codes are a reset path, not a second factor). The exact standard wording is quoted when the server policy document is written (M2.2).
  - **Blocklist:** a list of common and compromised passwords stored locally with the server and checked in memory at registration, change and recovery. No registration-time external API and no network call. Rejected also: the login identifier itself (as submitted and normalized, with common case and trivial-substring variants) and the product name `TaskNest`. Before it is added, the server documentation records the list's **source, licence, version and update procedure** (a reviewed file replaced by a commit, never fetched at run time). Tests cover: a listed password refused, a context value refused, an unlisted password accepted, and the list loading from the packaged file. Adding the list is a data dependency that is approved with M2.2.
  - Existing passwords are only normalized at login, never re-checked against the policy, so a later policy change cannot lock anyone out.
- **Tokens:** 15-minute opaque access token. Refresh tokens are opaque 256-bit values, stored hashed, bound to a device, rotated on every use; reuse of a rotated token revokes the whole family.
  - **Lifetime (decision 15):** a refresh token slides 90 days from each rotation, but never beyond **180 days after the family was created**. Past either limit the client is answered `401` and signs in again; local data is untouched.
  - **Interrupted rotation (design A, approved):** if the client never stored the new token (crash, dropped connection), its old token counts as reuse, the family is revoked and the user signs in again. No grace window; no successor tokens are stored.
  - **Access tokens follow the family:** an access token authorizes a request only while its device's refresh-token family is neither revoked nor expired. The server checks this on every request and never issues an access token that outlives its family, so an expired or revoked device loses access at once and cannot keep a slot's worth of valid access. Every login creates a new device record (no client-held device ids in v1).
- **Devices (decision 16):** at most **25 active devices**. A device is active while its refresh-token family is neither revoked nor expired. Sign-in over the cap returns `409 device_limit_reached` with `details {limit, active}`; no session is created and nothing is revoked silently. The user revokes a device (`DELETE /v1/devices/{id}`) and signs in again. Recovery revokes every session, so it frees all slots.
- **Client storage:** Windows Credential Manager and Android Keystore, never SQLite, settings, logs or command lines. A Windows credential blob is limited to 2560 bytes, which these tokens fit. A keychain library would be a third-party dependency and needs approval; Win32 `CredRead`/`CredWrite` do not.
- **Isolation:** every identifier is scoped to the token's account; another account's id behaves exactly like an unknown id. Server tests must try cross-account access on every endpoint. Row-level security is not relied on (not portable).
- **Recovery codes:**
  - Ten single-use codes, 16 base32 characters (80 bits), returned once at registration and on regeneration.
  - Stored as HMAC-SHA256 with a server-side pepper from the environment plus a non-secret code id. A slow hash adds nothing at this entropy; the pepper protects against a stolen database.
  - Constant-time comparison; rate limits per account and per address.
  - Using a code sets a new password and revokes every session and token family. Local data on devices is untouched.
  - If password and all codes are lost, the account cannot be recovered: there is no email and no operator override. Each device still holds a full local copy, so the user creates a new account and uploads from a device (SYNC 9.2). The old cloud account remains until the user deletes it (decision 9). The UI must explain this at registration, before the codes can be dismissed.
- **Login identifier:** an unverified login name (email format allowed, never mailed). Registration answering "taken" allows login-name enumeration; this is accepted for v1 and mitigated by rate limits.
- **Abuse limits:** TLS only, rate limits on authentication and sync, no task content in logs, payload and field limits (SYNC 12).
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
| **M1** Local prep (no network) | Schema v4, UUIDs, bases, tombstones, outbox, transactional mutations | Migration keeps every row (tested on copies of real databases); bases are full snapshots written only from server copies; the response-acknowledgement transaction and the conflict table follow SYNC 11.10 to 11.12; a failed mutation leaves no outbox row; every **synced** mutation path (add, update, setCompleted, delete, restore, purge, "Immediately", "Clear completed") writes exactly one operation or folds into an unsent row; **`markReminderSent` is local-only: it writes no outbox row and leaves `server_rev` and the base unchanged**; undo of a sent or in-flight delete covered; client fixtures for the outbox, apply-pull and local-only cases pass in Qt Test | **Not approved** |
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

Status: only **M2.0 (these documentation edits)** is approved. Nothing in M2.1 onward has started: no `server/` directory, no dependency, no PostgreSQL, no Docker. M1 and SQLite schema v4 are untouched. M2 contains no sync endpoints and no sync tables (decision 12).

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

**Design A: strict rotation, re-login.** Presenting an already-rotated token always revokes the whole family. The device is signed out and the user signs in again, which creates a new device row (the revoked one stops counting toward the cap). Local data is untouched.

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

### 16.5 Proposed API documentation changes (not applied)

Shown separately for review (`api/` is not modified): `NewPassword` schema (15 to 128 code points after NFC) used by register, recover and change-password; login password only normalized; error code and `409 DeviceLimit` response for login; refresh lifetime wording (90 sliding, 180 absolute); device wording (active devices, revoke frees a slot, revoking the calling device equals sign-out); SYNC 6.3 retention disclosure with the M3 purge note. The existing `GET /v1/devices` and `DELETE /v1/devices/{id}` operations already exist in the contract.


### 16.6 Access tokens, refresh-token families and devices (data model and checks)

**Tables (M2 baseline, PostgreSQL; all ids UUIDs generated in application code).**

| Table | Key columns | Notes |
|---|---|---|
| `users` | `id` pk, login (normalized, unique), password hash | |
| `devices` | `id` pk, `account_id` fk, name, platform, client_version, `created_at`, `last_seen_at` | One row per sign-in (every login creates one). |
| `refresh_families` | `id` pk, `account_id` fk, `device_id` fk **unique** (one family per device), `created_at`, `absolute_expires_at` (= `created_at` + 180 days), `sliding_expires_at` (starts at +90 days, moved on each rotation but never past `absolute_expires_at`), `revoked_at` null, `revoked_reason` null; unique `(id, account_id)` | The family is the single source of truth for whether a device session is alive. |
| `refresh_tokens` | `id` pk, `family_id` fk, `generation`, `token_hash` unique (SHA-256 of the 256-bit value), `created_at`, `rotated_at` null | Only the latest generation (`rotated_at` null) may be rotated. A row with `rotated_at` set that is presented again is reuse. |
| `access_tokens` | `id` pk, `token_hash` unique, `family_id` not null, `account_id` not null, `issued_at`, `expires_at`; **composite fk `(family_id, account_id)` references `refresh_families(id, account_id)`** | Opaque value stored only as a hash. `expires_at` is the earliest of issue time + 15 minutes, `sliding_expires_at` and `absolute_expires_at`. |

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

**Lifecycle rules.**
- **Login:** in one transaction, lock the `users` row (`SELECT ... FOR UPDATE`) so two concurrent logins cannot both take the last slot; count active devices (below); at 25, answer `409 device_limit_reached` and write nothing; otherwise insert device, family, first refresh token and one access token.
- **Refresh:** find the token by hash. If it is the latest generation and its family is alive, mark it rotated, insert the next generation, move `sliding_expires_at` (capped at `absolute_expires_at`), **delete the family's existing access tokens** and issue one new one (one live access token per family; an in-flight request using the replaced token gets `401`, and the client retries with the new token). If the token is already rotated, set `revoked_at` on the family (reason `reuse`) and answer `401`. If the family is revoked or expired, answer `401`.
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
| **Old access token after a subsequent login** | Device A signs in, then device B signs in (a new device and family). A's token still authorizes as A only (`current` is A in `GET /devices`) and B's as B only; neither ever authenticates as the other. After A refreshes, A's previous access token is refused and only the new one works. Recovery after both logins refuses both. |
| **Access token of a different family** | A's token cannot act as B: `DELETE /devices/{B}` with a token of another account gives `404`; a token row cannot be stored with a family of another account (composite fk violation); presenting B's refresh token with A's access token has no effect on A's session; A's token is not accepted after A's family is revoked even though B's family is alive. |
| **Cap and races** | 25 active devices admit no 26th (`409`, no session created, nothing revoked); revoking one lets a login succeed; an expired or revoked device does not count; two concurrent logins at 24 devices yield exactly one success. |
| **Strict rotation** | Using a rotated refresh token revokes the family and the family's access token stops working at once; no grace window. |
