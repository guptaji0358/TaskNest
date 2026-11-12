# Conformance fixtures

Shared by the Python server tests (`server/`, pytest) and the Qt client tests (`tests/`, Qt Test). Both read these files from `api/conformance/` at test time; neither copies them. The rules they exercise are in `../SYNC.md`; the wire format is `../openapi.yaml`. Files are LF, UTF-8, no BOM (`../.gitattributes`).

A fixture is the specification by example: if a fixture and `SYNC.md` disagree, that is a bug in one of them and must be resolved before either implementation changes.

## `server_scenarios.json`

`{ "format": "tasknest-conformance-server/1", "scenarios": [ ... ] }`. Each scenario starts with an **empty server** (epoch `epoch-1` unless `server_epoch` is set) and runs its steps in order.

| Field | Meaning |
|---|---|
| `id`, `description`, `rules` | Name, one sentence, and the `SYNC.md` sections it exercises. |
| `devices` | Map of a letter to `{ "id": <uuid>, "account": <name, default "u1"> }`. Devices of the same account share data; different accounts are isolated. |
| `steps[].device` | The letter of the calling device. The server derives the device from the credentials, as in production. |
| `steps[].now` | Optional. Sets the server clock (RFC 3339, milliseconds, `Z`) from this step on. The server clock is the only clock the scenarios use. |
| `steps[].push` | `{ "ops": [...] }`, exactly the `PushRequest` body. |
| `steps[].pull` | `{ "cursor": n, "limit": n? }`, the query of `GET /v1/sync/pull`. |
| `steps[].expect` | Expected response, compared as a **subset** (below). `http_status` (default 200) is the expected HTTP status and is not part of the compared body; a non-200 body is the error envelope (for example `cursor_ahead`). |

Matching rules for `expect`:

- Objects: every key in `expect` must be present and match; extra keys in the actual response are ignored.
- Arrays: same length, compared element by element (each element as a subset).
- The string `"$absent"` as a value means the key must not be present (used to prove tombstones carry no content).
- The string `"$any"` matches any value.
- Conflict records are matched on the listed keys only. Their `id` and `detected_at` are not asserted.
- Numbers, strings and booleans must be equal. Timestamps in expectations come from the scenario's own `now` values.

Conventions in the data: task ids are `10000000-0000-4000-8000-0000000000NN`, operation ids `e0000000-0000-4000-8000-0000000000NN`, device ids `d0000000-0000-4000-8000-00000000000a` / `b` / `c`. Default test task: title `Task N`, list `Inbox`, created `2026-01-01T10:00:00.000Z`.

A server implementation passes when every scenario in the file produces no mismatch. It SHOULD also validate each request and response against `openapi.yaml`.

## `client_cases.json`

`{ "format": "tasknest-conformance-client/1", "cases": [ ... ] }`. Each case is a pure function of its `input`; no network and no database are involved, so the Qt tests can run them against the real outbox and apply-pull code with an in-memory or temporary database. `expect` uses the same subset matching, including `"$absent"`.

| `kind` | Input | Output compared | Rule |
|---|---|---|---|
| `outbox_write` | `outbox` (rows), `op` (new row) | `outbox` | SYNC 11.2 |
| `outbox_undo_delete` | `outbox`, `task_id` | `outbox`, `cancelled` | SYNC 11.3 |
| `outbox_compact` | `outbox` | `batch` | SYNC 11.4 |
| `apply_pull` | `local` (`tasks` map of `{task, server_rev}` and `outbox`), `change` (one pull item) | `local` | SYNC 11.5, 7.3, 7.5 |
| `rebaseline` | `local_tasks` (full tasks; an optional `base` is the last server copy the device saw), `server` (pull items) | `adopt` (task to adopted rev), `ops`, `held`, `conflicts` (local values preserved), `take` (groups taken from the server) | SYNC 9.2, 10 |
| `apply_result` | `local`, `op_id`, `result` (one push result) | `local` | SYNC 11.7, 4.3, 4.4 |
| `fold_old` | `task`, `rows` (rows older than 60 days, with `queued_days_ago`), `base`, `server` (one pull item or null) | `ops`, `conflicts`, `held`, `satisfied`, `task_restored` | SYNC 11.6 |
| `held_transition` | `local`, `task_id`, `event` | `local`, `ask`, `reconcile` | SYNC 11.8 |
| `local_mutation` | `local`, `mutation` (`markReminderSent`) | `local` (unchanged: no outbox row, same `server_rev`) | SYNC 3.2, 11.1 |
| `local_edit` | `local`, `task_id`, `op_id`, `edit` (fields to change), optional `restore_of` | `local` (task changed, **base unchanged**, one outbox row written or folded) | SYNC 11.2, 11.10 |
| `apply_response` | `local`, `response` (a whole push response), optional `fault`, optional `request_link_generation` | `outcome`, `local`, `next_batch` (op ids an immediate re-send would begin with) | SYNC 11.11 |
| `apply_response_seq` | `local`, `steps` (each a `response` plus optional `fault`) | `steps[]` (each as `apply_response`, then the next step starts from its `local`), final `local` | SYNC 11.11, 4.4 |
| `reconcile_apply` | `local`, `server` (one pull item), `steps` (each optional `fault: "before_commit"`) | `steps[].local`, final `local` | SYNC 9.2, 11.12 |
| `conflict_store` | `records`, `new` | `records` (insert-or-ignore by the uniqueness rule) | SYNC 11.12 |
| `conflict_resolve` | `local`, `record_id`, `action` (`keep`, `restore`, `restore_confirmed`, `delete`), `op_id`, `now` | `result` (`kept_cloud`, `queued`, `needs_confirm`, `unavailable`, `deleted`), `current`, `local` | SYNC 11.12 |
| `conflict_cleanup` | `records`, `now` | `records` | SYNC 11.12 |

Outbox rows in the cases are abbreviated (`op_id`, `type`, `task_id` and only the fields the case needs); the Qt harness maps them onto its own row type. A row may carry `in_flight: true`, or `after: <op_id>` (its `base_rev` is unresolved until that in-flight row is answered, SYNC 11.2). In `local`, a task row is `{task, server_rev, base, held?, not_synced?}`. `base` is `{task: <full snapshot>}` of the last server copy the device adopted; its rev is `server_rev` (SYNC 11.10). It is always a full snapshot, never a digest. Cases whose outcome depends on the base carry it; a row without `base` is one whose base the case does not examine. Optional `local` members: `conflicts` (local conflict records, SYNC 11.12), `cursor`, `epoch`, `link_generation`, `device_id`. Outbox rows may also carry `authored_rev`, `collided`, `restore_of`, `attempts` and `sent_payload` (opaque here). A `fault` names where a simulated process death or error happens: `before_apply` (response received, no transaction), `during_result: N` (while applying result N), `before_commit`, `after_commit`; every fault except `after_commit` must leave the stored state exactly as before the call.

## How these files were validated

Nothing may be installed at this stage, so the fixtures were checked with throwaway stdlib-plus-PyYAML scripts kept outside the repository: a reference model of `SYNC.md` runs every server scenario and every client case, every request operation is checked against the `Op` schema (11 operations are deliberately schema-invalid and rejected by server rules), and every model response against `PushResponse` / `PullResponse`. The real harnesses arrive with M3 (server) and M4 (client). Passing proves the fixtures and rules agree with each other, not that the rules are right; independent implementations passing them is the real evidence. For revision 3 the model was also **mutated** (ten deliberate rule violations: base taken from the local row, collision check removed, non-atomic acknowledgement, a commit despite a fault, restore on the wrong revision, restore without its guard, cleanup of open records, no de-duplication, a local edit moving the base, a non-strict restore row); every mutant makes at least one case fail.
