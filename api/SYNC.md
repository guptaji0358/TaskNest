# TaskNest sync and conflict rules (protocol v1)

Normative. `openapi.yaml` is the wire format, `conformance/` holds executable examples of the rules below, and `../CLOUD.md` holds the reasoning and decisions. Section numbers are referenced from the fixtures; do not renumber them without updating `conformance/`.

Words: **MUST**, **MUST NOT** and **SHOULD** are used as in RFC 2119. The server is the only authority that orders changes. Clients never use their own clocks to decide who wins.

## 1. Scope and vocabulary

- Sync is optional. With it off, or before an account is linked, TaskNest never contacts a server and behaves exactly as it does today.
- **Account**: one login. **Device**: one signed-in installation; every sign-in creates a fresh device record.
- **Entity**: a *task* (identified by a client-generated UUID) or a *setting* (identified by a fixed key).
- **rev**: per-entity revision, starting at 1 for a task, +1 on every applied change. A setting that was never set has rev 0.
- **seq**: per-account change sequence. Every applied change takes the next value, with no gaps; no-ops and rejected operations take none. It is assigned inside the same database transaction as the change, serialised per account, so a change with a lower seq is never visible after one with a higher seq.
- **Tombstone**: the server's record that a task was deleted. It keeps the id, rev, seq, `deleted_at`, `delete_reason`, the deleting device and the immutable `created_at` (a timestamp, kept server-side only and not sent in pull), and **no task content**. A task's rev never decreases or restarts over its life: deletion, resurrection and restore each add 1. Tombstones are kept for the life of the account in v1 (no compaction), so the server can always tell "deleted" from "never existed".
- **Outbox**: the client's local journal of operations not yet acknowledged.
- **Base** of a local task: the last server copy this device has adopted, stored as a **full snapshot** (every field of 2.1 exactly as the server returned it) together with its rev. Digests are not used: the "before" value is needed to show and restore what changed. It is written only from a server-supplied copy and never from the local row, so an unacknowledged local edit cannot move it; the exact events are in 11.10. It is the third side of every reconciliation (9.2, 11.6) and is kept across unlink, relink and epoch changes; it is trusted as a *revision* only for the same account and epoch.
- **Waiting operation**: an outbox row that is `pending` or `in_flight` (not yet acknowledged).
- **Conflict record (local)**: a row in the device's own conflict table that preserves a value the device did not keep, or a notice delivered by the server. Complete schema, restore and cleanup rules are in 11.12.

## 2. Data model

### 2.1 Synced task fields

`id` (UUID), `title`, `notes`, `list_name`, `priority`, `due_at`, `completed`, `completed_at`, `reminder_at`, `created_at`. Not synced: the local integer id (it stays device-local, so `tasknest://task/<id>` links are device-local), `reminder_sent` (per device, so every device rings; a changed `reminder_at` re-arms it, as today), and `updated_at` (informational and local).

### 2.2 Field groups

Merging is per group, not per task:

| Group | Fields |
|---|---|
| `title` | title |
| `notes` | notes |
| `list_name` | list_name |
| `priority` | priority |
| `due_at` | due_at |
| `reminder_at` | reminder_at |
| `completion` | completed **and** completed_at (always move together) |

`created_at` is immutable after creation. For each live task the server stores, per group, the rev at which it last changed and the device that changed it.

### 2.3 Settings

Account-wide settings sync like tasks but are a single value each. v1 has one key, `completed_cleanup_policy` (section 8).

## 3. Operations

### 3.1 Types

| Type | Meaning | `base_rev` |
|---|---|---|
| `create` | A task the server has not seen. | null |
| `update` | A deliberate user edit. | the task rev the client last saw (≥ 1) |
| `delete` | Delete, with `reason` `user`, `retention` or `clear_completed`. | the task rev the client last saw |
| `restore` | Bring a tombstoned task back (undo of a sent delete, or "keep this task"). | the tombstone's rev |
| `set_setting` | Set an account setting. | the setting rev last seen (0 if never set) |

Every operation has an `op_id` (UUID, unique for the account). `update` also carries `origin: "user"`, the groups that changed (`changed`, non-empty) and the client's full task state (`task`).

### 3.2 What counts as a deliberate edit

Only a user action creates an `update`: editing a field, completing, reopening, moving to a list, setting a due date or reminder. These never create one: applying a pulled change, a reminder being marked sent (`markReminderSent` is local-only: no operation, no outbox row, no `server_rev` change), a retention purge, the app starting, or a device merely **still holding** a task. An `update` with an empty `changed` list is invalid. This is what stops an old offline copy from resurrecting a purged task (section 7).

### 3.3 Validation

An operation that breaks these rules is `rejected` with `validation_failed` (or `empty_changes`) and consumes no seq:

- `task.id` equals the operation's `task_id`. The task has exactly the fields in 2.1. This applies in full to every operation that carries a task (`create`, `update`, `restore`), including when an `update` resurrects (7.3). `title` is trimmed and then 1–500 characters; `list_name` is trimmed, 1–100; `notes` ≤ 100 000; `priority` 0–2; `completed` is a boolean.
- Timestamps are `YYYY-MM-DDTHH:MM:SS.mmmZ`, years 1000–9999.
- `completed_at` is null unless `completed` is true. (`completed` true with null `completed_at` is allowed: legacy tasks have an unknown completion time.)
- `changed` names only known groups, with no repeats. `update` requires `origin` `user`. `create` has no `base_rev`. `delete` has a valid `reason`. A setting key is known and its value matches `never`, `immediately` or `days:N` with N 1–3650.
- The server returns the canonical task (after trimming); clients adopt it.

### 3.4 Server clamps

The server never trusts client clocks for ordering, but it does bound two client-supplied times, because retention uses them: `created_at` greater than the server's now is set to now; `completed_at` is raised to `created_at` if earlier and lowered to now if later. Changed fields are listed in the result's `adjusted`.

## 4. Push

### 4.0 Batches and idempotency

`POST /v1/sync/push` carries up to 200 operations, applied in order. Each operation is atomic and independent: a rejected one does not stop the rest. The server remembers every `op_id` (at least 90 days) with its result; re-sending an `op_id` returns the stored result with `replayed: true`, whatever the task looks like now. The device comes from the access token, never from the body. Results, including `rejected` and `noop`, are stored in the same transaction as the operation's effects, so a remembered `op_id` means the operation was decided and the stored decision never changes on replay.

### 4.1 `create`

| Server state of the id | Result |
|---|---|
| unknown | `applied`, rev 1 |
| live, content identical | `noop` |
| live, content different | `rejected` `already_exists` (returns the current task) |
| tombstoned | `rejected` `already_deleted`. A create never resurrects. |

### 4.2 `update` (live task)

1. `base_rev` greater than the task's rev: `rejected` `invalid_base_rev`.
2. For each group in `changed`, compare the client's value with the stored value. Equal: nothing to do for that group (convergent edit).
3. A group whose stored value differs **and** whose last-changed rev is greater than `base_rev` is a **collision** (section 6.2). Other differing groups apply cleanly.
4. If no group differs: `noop`, no seq. Otherwise the task takes rev+1 and the next seq, the applied groups record the new rev and the device.

Unknown id: `rejected` `unknown_task`. Tombstoned id: section 7.3 and 7.5.

### 4.3 Result statuses

| Status | Meaning |
|---|---|
| `applied` | Applied on the revision the client saw. |
| `merged` | Applied; other devices changed other groups since `base_rev`. The returned `task` includes their changes. |
| `conflict_resolved` | Applied; at least one group collided and the incoming value won. See `conflicts`. |
| `resurrected` | An edit brought a deleted task back (7.3). |
| `restored` | A `restore` brought a task back (7.6). |
| `noop` | Nothing changed (identical content, duplicate delete, repeated setting). |
| `rejected` | Not applied; `code` says why. |

Rejection codes and the client's response:

| Code | Client action |
|---|---|
| `validation_failed`, `empty_changes` | Do not retry. Keep the task locally, mark it "not synced" and show the error. |
| `unknown_task` | Reconcile this task (9.2): the server has no copy, so send a `create` with a new `op_id`. If that answers `already_deleted`, hold. |
| `already_exists` | Reconcile this task by the three-way rule of 9.2 against the returned task. Never send the server's rev as `base_rev` for content the device has not seen. |
| `already_deleted` | Hold the task locally and ask the user (11.8). |
| `invalid_base_rev` | The rejection is stored under its `op_id` (4.0), so a retry MUST use a **new** `op_id` (the outcome is known, 4.4 does not apply). Pull, rebase the pending operation (11.5) and retry once; the row keeps its place in the outbox and the rows behind it. If it is rejected again, stop retrying and reconcile the task (9.2); the local content is untouched. A `restore` is never retried: the Keep is void, the task stays held and the user is asked again (11.8) once a pull has refreshed the tombstone revision. |
| `not_deleted` | Drop the operation and adopt the returned task. |
| `edit_won` | A delete lost to a newer edit (7.4): restore the row from the returned task and show a notice. |

### 4.4 Ambiguous outcomes and replay

Operations commit independently, so a batch can be partly applied when a connection drops. Therefore:

- Only a `200` carries per-operation outcomes. A timeout, a closed connection, a malformed body, and any `5xx` (including 502/503/504 from a proxy) mean **the outcome of every operation in the batch is unknown**, not "nothing applied".
- `401`, `413`, `422` and `429` on `push` MUST be produced before any operation is applied; after one of these nothing was applied.
- On an unknown outcome the client re-sends the in-flight operations **byte-identical** (same `op_id`, same stored payload, same order; 11.7). Committed ones return their stored result with `replayed: true`, the rest are applied now. New operations may be appended after them, never inserted or altered.
- An operation is never re-sent under a new `op_id` while its outcome is unknown, and never once the server may have forgotten its `op_id` (11.6). Re-sending an old update with a new `op_id` could overwrite a newer change made after the first copy applied; such rows are reconciled instead (11.6).
- A replayed result describes the state at the time the operation was decided. The client applies it under the rules of 11.7 and relies on pull for anything newer.

## 5. Pull

`GET /v1/sync/pull?cursor=N&limit=L` returns entities whose seq is greater than N, ordered by seq (unique per entity), each at its **latest** state (a task changed three times appears once). Items are `task` (full state), `tombstone` (content-free) or `setting`.

### 5.1 Snapshot and high-water mark

- The server computes `head_seq`, the items and the conflicts of one response from **one consistent snapshot** (a single statement or a repeatable-read transaction). `head_seq` is the snapshot's high-water mark: every change with seq <= `head_seq` is visible to this response or superseded by a later item, and none appears later.
- A page holds the first L items after N. `next_cursor` is the seq of its last item; with no items it is `max(N, head_seq)`. `has_more` is true exactly while `next_cursor` < `head_seq`; on the last page `next_cursor` equals `head_seq`.
- The conflicts returned are those whose `change_seq` lies in (N, `next_cursor`] (at most 7 per change, not counted against L), so a notice is delivered even if its task changed again before the device pulled.
- A cursor greater than the account's `head_seq` means the server went backwards: it answers `409` `cursor_ahead`, and the client treats it as an epoch change (section 10).

### 5.2 Client paging

- The client keeps requesting with `cursor = next_cursor` while `has_more`. A full pull is a loop of pages; all pages of one loop must carry the same `server_epoch`, otherwise the loop is abandoned and section 10 applies.
- Each page's items **and** conflicts are applied, and the cursor is stored, in one local transaction. A crash re-reads the page; applying is idempotent because of the rev rules in 11.5.
- An entity that changes between pages is simply delivered again at its higher seq. The loop ends only when `has_more` is false; only then is the pull complete (for 9.2, "the account's whole state" means a completed loop).

The pull cursor moves **only** through pull. A push result's `seq` MUST NOT advance it, or changes from other devices in between would be skipped. A client's own pushed changes come back in its next pull and are recognised by an equal rev (11.5).

## 6. Conflict rules

### 6.1 Different groups merge

Concurrent edits to different groups are both kept (`merged`). There is no conflict record.

### 6.2 Same group: the later arrival wins, the loser is kept

When two devices change the same group from the same base, the operation the server applies last wins. The overwritten value is stored in a **conflict record** (`kind: field`) with the losing and winning values and devices, and is delivered to the losing device. This applies to `title`, `notes`, `list_name`, `priority`, `due_at`, `reminder_at` and `completion`. `notes` is not text-merged in v1; the overwritten text is in the record so the user can restore it. If both devices set the same value there is no conflict and no change.

Arrival order is a deliberate choice: it is deterministic and independent of device clocks. A stale offline edit can therefore overwrite a newer one; the conflict record and notice exist to make that recoverable. `client_edited_at` is informational only and is shown next to a notice, never used to decide.

### 6.3 Conflict records and notices

A record has: `id`, `change_seq`, `kind`, `detected_at`, and as relevant `task_id`, `key`, `field`, `losing_value`, `winning_value`, `losing_device_id`, `winning_device_id`, `delete_reason`, `deleted_at`. They are kept 90 days and readable through `GET /v1/sync/conflicts`.

| Kind | Delivered to | Notice |
|---|---|---|
| `field` | the losing device | "<field> of <title> was changed on <device>; your value was replaced." with a restore action |
| `setting` | the losing device | same, for the cleanup policy |
| `edit_over_delete` | every device except the editor | "<title> was restored because it was edited on <device>." |
| `delete_lost_to_edit` | the deleting device, in the push result and in the history | "<title> was not deleted because it was edited on <device>." |

Clients de-duplicate notices by record id. Restoring a losing value is a new user edit (a new `update`).

## 7. Deletion, purge and resurrection

All deletions (user delete, "Clear completed", retention purge, "Immediately" cleanup) are `delete` operations with a `reason`. They are synced tombstones, so every device converges.

### 7.1 Delete

On a live task: `base_rev` equal to the task's rev applies (rev+1, next seq); the content is removed, only the tombstone remains. `base_rev` less than the rev is 7.4. `base_rev` greater than the rev is `invalid_base_rev`. Unknown id: `unknown_task`.

### 7.2 Duplicate delete

Deleting an already-tombstoned task is `noop` (returns the tombstone's rev and seq). Two devices purging the same task is therefore harmless.

### 7.3 A deliberate edit resurrects

An `update` for a tombstoned task whose `base_rev` is **less than** the tombstone's rev means the editor had not seen the deletion. Checks run in this order: `base_rev` greater than the tombstone's rev is `invalid_base_rev`; the operation is validated in full (3.3: the **whole** `task`, not just the `changed` groups, so a stale task cannot smuggle invalid content in); `created_at` is replaced by the tombstone's stored value (reported in `adjusted`) and `completed_at` is clamped (3.4). Then the server brings the task back from the operation's full `task` (the tombstone holds no content): rev = tombstone rev + 1, status `resurrected`, all groups attributed to the editing device, and an `edit_over_delete` record (naming the deleter, the reason and `deleted_at`). **Edit wins.** The unchanged groups of a resurrected task are whatever the editing device held; nothing newer exists, because the content was deleted.

### 7.4 A stale delete loses

A `delete` whose `base_rev` is less than the task's current rev means someone changed the task after the deleter last saw it. The delete is `rejected` with `edit_won`, the current task is returned, and a `delete_lost_to_edit` record is stored. This covers a retention purge racing a reopen or edit.

### 7.5 Presence is never an edit

- A `create` for a tombstoned id: `already_deleted` (4.1).
- An `update` for a tombstoned id whose `base_rev` is **at or above** the tombstone's rev: `invalid_base_rev`. An honest client that has seen the tombstone no longer holds the task.
- A device that merely still holds a purged task sends nothing. When it pulls the tombstone it removes its copy (11.5), unless it has a pending user edit for it (then 7.3 applies).

Together these mean an old offline device can only bring a purged task back by a real, identifiable user edit, validated against the server's deletion state, and the conflict record shows what happened.

### 7.6 Restore

`restore` carries the **complete task** the user wants back (`task`, required, as in `create`), because a tombstone holds no content. It is the undo of a delete that was already sent, or the user choosing "Keep" for a task deleted elsewhere (11.8).

Checks, in order: unknown id: `unknown_task`. Live task: `not_deleted` (returns the current task; nothing changes). `base_rev` different from the tombstone's rev (lower, or higher): `invalid_base_rev` (the deletion the user decided about is not the current one). Then full validation of `task` exactly as for `create` (3.3, including `task.id` = `task_id`), `created_at` replaced by the tombstone's stored value and `completed_at` clamped (3.4, reported in `adjusted`).

On success: result `restored`, rev = tombstone rev + 1, next seq, every group attributed to the restoring device, the canonical task returned, no conflict record (a restore is the user's own reversal; other devices receive the task through pull at a higher seq and re-insert it). A repeated `op_id` replays the stored result (4.0); a second `restore` with a new `op_id` finds a live task: `not_deleted`.

### 7.7 Tombstone lifetime

v1 keeps tombstones until the account is deleted. They hold no task content (title, notes and the rest are removed at deletion); only ids, revs, times, reasons, the deleting device and `created_at` remain. A compaction scheme, if ever needed, requires its own design and a protocol revision.

## 8. Account-wide cleanup policy

- While a device is linked, **one** policy applies to all devices: the account setting `completed_cleanup_policy`, same grammar as the local one (`never`, `immediately`, `days:N`). Unset means `never`.
- The device's own local policy (in `app_metadata`) is **not overwritten**. It is used again, unchanged, when the device is unlinked or signed out, so local-only operation stays independent.
- Changing the policy uses the existing preview-and-confirm dialog and states that it applies to all linked devices, then sends `set_setting`.
- On first link: if the account has no policy it stays `never` and the device's local policy is not uploaded; the UI may offer "Use this device's policy for the account" behind the usual confirmation. If the account has a policy it applies, with a one-time notice when it differs from the local one.
- A device receiving a changed policy applies it, shows "Cleanup policy changed to X on <device>", and runs cleanup at its normal triggers (startup, completion, window activation, policy save), never on a timer. The resulting deletions are `delete` operations with `reason: retention`.
- Concurrent policy changes follow 6.2 (`kind: setting`). The retention cutoff uses `completed_at` as clamped by the server (3.4).

## 9. Isolation and first link

### 9.1 Isolation

Every identifier is scoped to the account in the access token. An id that belongs to another account behaves exactly like an unknown id (`unknown_task`, or 404 for devices); the API never reveals that it exists. The same task UUID can exist independently in two accounts.

### 9.2 First link, relink, epoch change, and reconciliation

Used when a device first links, links again after being unlinked, recovers from an epoch change (section 10), or reconciles a task that cannot be sent as it stands (4.3, 11.6). The client first makes a byte-identical backup of its database, shows how many tasks will be uploaded and to which server, and waits for confirmation. Then it completes a full pull (5.2) and compares, per local task, **three** things: **L** the local copy, **B** its base (section 1; absent if the device never saw a server copy), and **S** the server state.

The revision the server holds for a task is **never** used as `base_rev` for content the device has not seen. `base_rev` is always a revision whose content the device actually adopted, or the current server revision for groups proven unchanged on the server (below).

| Server state | Action |
|---|---|
| unknown | `create` from L |
| live, S identical to L | adopt S and its rev as the new base, no operation |
| live, differs, **B known** | Per field group g (2.2): if L(g) = S(g): nothing. If L(g) = B(g) (the user did not change it): take S(g). If S(g) = B(g) and L(g) differs (only this device changed it): include g in an `update` (`changed`), with `base_rev` = S's rev; the server value equals what the device last saw, so nothing newer is overwritten. If L(g), S(g) and B(g) all differ: **keep S(g)** and save L(g) in a local conflict record `reconcile` (11.12; shown with a restore action; restoring is a new user edit). Send no operation for that group. The base becomes S, because every group of S now has a disposition (equal, taken, preserved in a record, or carried by the new `update`). |
| live, differs, **B absent** | Nothing proves who changed what. For every differing group **keep S(g)** and save L(g) in a local conflict record (11.12). Send no operation. Adopt S as the new base. |
| tombstoned | **Hold** (11.8): no operation, never resurrect. |

Each task's reconciliation (local row, base, conflict records, outbox row) is **one local transaction**; the conflict record is inserted in the same transaction that replaces the local value. A crash before commit leaves the old value and nothing else, and re-running the reconciliation is idempotent because records are de-duplicated (11.12). Nothing is ever matched or merged by title. A task is reconciled only against the same UUID. After the table, the cursor is set to the completed pull's final `next_cursor`; operations sent by the reconciliation come back through later pulls as echoes. If reconciling a task keeps server values over local ones, the UI says how many tasks had local values preserved, and where they are.

### 9.3 Signing in to a different account

If the database was linked to account X and the user signs in as Y, the client MUST NOT upload anything silently. It offers: sign back in as X, or unlink and start this device fresh against Y (local tasks stay and are then uploaded to Y through 9.2 after confirmation).

## 10. Server epoch

Every sync response carries `server_epoch`, a random value that changes if the server database was restored or rebuilt. A client that sees a different epoch than the one it stored: stops pushing, makes a backup, discards its outbox and cursor (cursor 0), keeps every task's base **content** but marks the stored revs as untrusted (they belong to the old epoch and the restored server may have reused those numbers), and runs 9.2 against the new state. Rows lost this way are not re-sent; 9.2 recomputes the user's changes as the groups where L differs from B. Known limitation: deletions made after the restored dump's date may reappear, because the restored server never saw them.

## 11. Client rules

### 11.1 Recording

The outbox and local tombstone records are written only once an account is linked, inside the **same transaction** as the local change, so a failed mutation leaves no operation behind. Before linking, TaskNest records nothing and is byte-for-byte unchanged in behaviour. On first link every local task is reconciled through 9.2. **Local-only mutations write nothing to the outbox:** `markReminderSent` (and the re-arming of `reminder_sent` when a pulled `reminder_at` changes), applying a pull, and changing the local cleanup policy. Every other repository mutation writes exactly one operation row (or folds into an unsent row under 11.2).

### 11.2 Writing a row

One row per operation, journal style. A row has a state: `pending` (never sent), `in_flight` (part of a batch whose outcome is not known) and carries `queued_at` (local clock; only used by 11.6), `attempts`, and, once in flight, `sent_payload` (the exact serialized operation). Write-time merging happens only where it is lossless and the earlier row is `pending`:

- `create` then `update` for the same task: the `create` takes the newer `task`.
- `update` then `update`: one `update` with the union of `changed`, the newer `task`, the original `base_rev`.
- `delete` is always appended, never merged.
- A row that is `in_flight` is never modified (its `sent_payload` is what a replay sends). A later operation for the same task is appended as `pending` with `base_rev` **unresolved** (`after: <op_id>`), and its `task` is the local copy at that moment. It is not sent, and no row of that task is added to a batch, until the in-flight row is resolved (11.7). Rows waiting behind the same in-flight row are appended one after another and are not merged with each other. Rows of other tasks are unaffected.
- Every `update` row records `authored_rev`: the rev of the task's base at the moment the row was written. It is the floor that `base_rev` falls back to when the base moves past a group the user also changed (11.10). A row that waits behind an in-flight row gets `collided` = true if a pull finds such a collision while it waits (its `base_rev` is still unresolved), so that resolution under 11.7 uses `authored_rev` instead of the new rev. A row created by a conflict restore carries `restore_of` (the record id, 11.12) and is never folded into or with another row.
- Writing or folding a row never changes the task's base (11.10).

### 11.3 Undo

The undo window is 8 seconds. Undoing a delete whose row has not been sent cancels that row (earlier rows for the task are untouched). If the delete was already sent, undo queues a `restore` carrying the full local task (7.6); if the delete is `in_flight`, the `restore` waits with an unresolved `base_rev` and is resolved under 11.7. Push is debounced for longer than the undo window (11.6), so most undone deletes never leave the device. Undoing the delete of a **held** task whose Keep (a `restore`) is in flight cancels the waiting delete and puts the task back as it was: still held, on the same in-flight `restore` and tombstone revision, with its base; edits made after the Keep are queued again behind that `restore`. The `restore`'s answer then applies as if the delete had never happened (11.7, 11.8).

### 11.4 Compaction when building a batch

For rows that are not in flight: `create` … `delete` for one task is dropped entirely (the server never saw it); `update` … `delete` becomes just the `delete`; everything else is sent in order. Rows for a task with an in-flight row are never dropped. Rows whose `base_rev` is unresolved (11.2) are not part of any batch yet.

### 11.5 Applying a pull

All of a page is applied in one local transaction together with the cursor (5.2). For each item, by kind:

- **task**, local row whose `server_rev` is **greater** than the item's rev: the item is older than what we already hold (the pull was computed before our own push result arrived); ignore it.
- **task**, no local row: insert it with the item's rev as its base. (A task this device removed because of a tombstone and that is held, or has a local tombstone record, is handled by 11.8.)
- **task**, local row with an equal rev: the echo of our own push; change nothing.
- **task**, local row without pending or in-flight rows: overwrite with the server copy; the copy and rev become the base.
- **task**, local row with pending rows (or an in-flight row): rebase. The server copy becomes the base (11.10). The local task is the server copy with the `changed` groups of the pending and in-flight rows re-applied from the local copy. A pending row's `task` is updated to match, and its `base_rev` moves to the new rev **only if none of its groups collides** with the server copy (11.10); otherwise it keeps its `base_rev`, so the server sees the concurrent change and applies 6.2. A pending row created by a conflict restore is cancelled instead of colliding (11.12). An in-flight row keeps its `sent_payload` untouched; its result is applied under 11.7 and rebases again.
- **tombstone**, no local row: ignore.
- **tombstone**, local row with a pending `delete`: our delete is satisfied; remove the row and the operation.
- **tombstone**, local row with a pending or in-flight `update`: **edit wins**. Keep the task; the operation stays queued and will resurrect it (7.3).
- **tombstone**, local row held (11.8): record the tombstone's rev as the one a later `restore` must use.
- **tombstone**, local row with no pending operation: the task was deleted elsewhere; remove it locally. (Make a backup first if the count is large.)
- **setting**: store as the account's effective value (section 8).

Local deletions made by applying a pull are not user actions and create no operations.

### 11.6 When to sync

Only on events, never on a polling timer: a debounced push (at least 10 seconds after the last local change, which is longer than the undo window); a pull when the app window is activated; after every successful push; and on "Sync now". After a failure, retry with exponential backoff (5 s, doubling, capped at 15 minutes, ±20 % jitter) using a single-shot timer that exists only while there is something to send or the last attempt failed. The first request after idle uses a 90-second timeout (free hosts sleep); later requests 30 seconds. The UI shows state: Off, Idle, Syncing, Waiting for server, Offline, Needs attention (with the error). Sync failures never block local use. Rows whose `queued_at` is older than 60 days are never sent (the server remembers op ids for 90 days; 60 leaves a margin for clock error and retries). They are reconciled instead, as follows. A wrong local clock only costs efficiency, because reconciliation is always safe.

#### Rows older than 60 days

For every task that has such a row, the client removes **all** of the task's rows that are not `in_flight`, and also the `in_flight` ones whose outcome has stayed unknown for more than 60 days, and records their effect as the *user's intent*: the union of the `changed` groups (current values come from the local task), or "delete", or "create". It then pulls to completion and reconciles the task by 9.2 with L = the local task and B = its base. Nothing is re-sent under an old `base_rev` or reused `op_id`. Outcomes:

| Intent | Server state | Result |
|---|---|---|
| groups changed | live | the three-way table of 9.2: groups unchanged on the server are sent in a fresh `update` (`base_rev` = current rev); groups changed on both sides keep the server value and the local value is preserved in a local conflict record |
| groups changed | tombstoned | hold (11.8); the local content is kept, nothing is resurrected automatically |
| groups changed | unknown | `create` from the local task |
| delete (any reason) | tombstoned | satisfied, drop |
| delete | live and equal to the base | fresh `delete` with `base_rev` = current rev |
| delete | live and changed since the base | the delete is dropped (edit wins), the local row is restored from the server copy and the user is told (as `edit_won`) |
| delete | unknown | nothing to do |
| create | unknown | fresh `create` |
| create | live | no base: keep the server copy, preserve differing local groups (9.2) |
| create | tombstoned | hold |

No user edit is lost: every local value that is not sent is either equal to the server value or saved in a conflict record with a restore action.

### 11.7 Push results, in-flight recovery and rebasing

Batch life cycle. The client builds a batch (11.4), and in **one local transaction** marks its rows `in_flight` and stores each row's `sent_payload`; only then does it send. Only one batch is in flight per account.

| Event | Client action |
|---|---|
| App starts, or a request failed with an unknown outcome (4.4) | In-flight rows stay in flight. The next batch begins with them, byte-identical from `sent_payload`, then any new `pending` rows whose `base_rev` is resolved. Backoff (11.6) applies. |
| `401`, `413`, `422`, `429` | Nothing was applied. Rows return to `pending` (a `413` splits the batch). A `422` is a client bug: stop and show "Needs attention". |
| `200`, result per op (replayed or not) | Apply the **whole response in one local transaction** (11.11): each result in order, as below, deleting each acknowledged row in that same transaction. |

Applying one result (inside that transaction):

- `applied`, `merged`, `conflict_resolved`, `resurrected`, `restored`, `noop` with a `rev`: set the task's `server_rev` to that rev and adopt `task` (or the tombstone) as its base **unless** a newer base is already stored (rev lower than the stored `server_rev`: keep the stored one; this happens for replays). Update the local row from the returned canonical task for groups that have no pending change; groups with pending changes keep the local value (rebase as in 11.5). Then resolve dependent rows: each later row of the same task gets its `base_rev` under the collision rule of 11.10 (the rev now stored, or its `authored_rev` if one of its groups collides with a change the acknowledged operation did not make), and its `task` rebased onto the returned server copy with its own `changed` groups re-applied.
- A delete's `applied`/`noop` gives the tombstone rev; a dependent `restore` takes it as `base_rev` and is then sent.
- `rejected`: follow the table in 4.3. A rejection never changes the base, and a `task` returned with a rejection is only **offered**: it becomes the base only where 4.3 says it is adopted (`not_deleted`, `edit_won`) or after the reconciliation of 9.2 has given every group a disposition (`already_exists`). Dependent rows of the same task follow the fate of the row they depend on: if it is dropped or the task is held or marked "not synced", they are dropped or held with it, and the local task content is untouched. `edit_won` additionally drops a dependent `restore` (the task is already live again).
- Conflict records in the result are stored and notified like those from pull; notices are de-duplicated by record id.

The cursor is never touched by a push (5).

### 11.8 Held tasks

A task is **held** when the server has deleted it and this device holds a copy that is not a pending deliberate edit: `already_deleted` on a `create`, a tombstone found by 9.2 or 11.6, or a pulled tombstone for a locally held task. Held is persisted per task (`held` = `deleted_in_cloud`, with the tombstone rev and `deleted_at`).

- While held: the task stays visible and locally editable; **no operation is queued** (edits do not resurrect it; the user decides first); pulled tombstones only update the recorded rev.
- **Keep**: queue a `restore` with the full current local task and `base_rev` = the recorded tombstone rev. On `restored` the hold ends and the base is set (11.7). On `invalid_base_rev` the recorded tombstone is out of date: pull, and if the task is live elsewhere reconcile it (9.2), if it is tombstoned again ask the user again (the decision was about a different deletion). On `not_deleted` adopt the returned task and reconcile (9.2).
- **Delete locally**: remove the row (with a backup first if the user chose it for many tasks at once) and discard the hold. No operation; the server already has the tombstone.
- The hold also ends if a pull shows the task live again (resurrected elsewhere): reconcile (9.2).

A separate flag `not_synced` (reason `validation_failed` or `empty_changes`) marks a task whose operation was rejected for content reasons: the operation is removed from the outbox, the error is shown, and the next deliberate edit of that task queues a fresh `create` (never accepted) or an `update` with `changed` = the groups where the task differs from its base. Unlinking clears holds and `not_synced` flags; relinking recomputes them (9.2).

### 11.9 Required tests for this section

The cases in `conformance/` (`client_cases.json`, kinds `outbox_*`, `local_edit`, `apply_pull`, `rebaseline`, `reconcile_apply`, `apply_result`, `apply_response`, `apply_response_seq`, `fold_old`, `held_transition`, `local_mutation`, `conflict_store`, `conflict_resolve`, `conflict_cleanup`) and the server scenarios `ambiguous-*`, `restore-*`, `resurrect-*` and `pull-*` are the executable form of 4.4, 5, 7.3, 7.6, 9.2 and 11.5 to 11.12. Both suites MUST pass them before the milestone that implements the rule (M3 server, M4 client, M1 for the outbox write rules, the base and the local conflict table) is accepted.

### 11.10 Base snapshots: when they change

**Storage (logical).** Table `sync_base`: `task_uuid` (primary key), `rev`, `snapshot` (full JSON of the fields in 2.1), `epoch` (the `server_epoch` it was received under), `trusted` (0 after an epoch change, section 10). `tasks.server_rev` is not a second source of truth: it is always equal to `sync_base.rev` and is written in the same statement group. A task that was never on the server has no row (`server_rev` null).

**Invariants.**

- **B1 (source).** `snapshot` and `rev` are written only from a server-supplied copy: a pull item, the `task` of a push result, or a copy adopted under 9.2. They are never copied from the local task row.
- **B2 (pair).** `rev` and `snapshot` always change together, in one transaction, and only forward within an epoch (a copy with a lower rev than the stored one is ignored, 11.5).
- **B3 (nothing unrepresented).** For every field group g of a synced task, either local(g) = base(g), or g is carried by a waiting operation (`changed` of an `update`, or the whole task of a `create` or `restore`), or the task is `held` or `not_synced` (11.8). A value that was dropped in favour of the server's is not a local value any more; it lives in a conflict record (11.12). A test oracle can check B1 to B3 after every step.

**Events.**

| Event | Base |
|---|---|
| A local edit, completion, reopen, undo, delete; writing, folding or cancelling an outbox row; marking a row `in_flight`; sending; a timeout, `5xx`, `401`, `413`, `422`, `429`; backoff | **Unchanged.** Only the local task row and the outbox move. Every queued update keeps the base it was written against, however many are queued. |
| Pull item newer than the stored rev (11.5), with or without waiting rows | Replaced by the item (rev and snapshot). The local task becomes the item plus the waiting rows' groups. |
| Pull item with an equal rev (an echo), or an older one | Unchanged. |
| Push result `applied`, `merged`, `conflict_resolved`, `resurrected`, `restored` or `noop`, rev above the stored rev | Replaced by the result's `task` and `rev` (the server's canonical copy as of that decision). It includes the acknowledged operation's own values and any other device's merged changes, **never** the values of operations still waiting. |
| The same, rev equal to or below the stored rev (a replay, or a pull got there first) | Unchanged. |
| Push result `rejected` | **Unchanged.** The offered `task` is not adopted by the rejection itself (11.7). |
| `not_deleted`, `edit_won` | Replaced by the returned task (the local row is replaced with it, so adopting it is complete). |
| Reconciliation (9.2, 11.6) | Replaced by S once every group has a disposition. |
| Tombstone pulled for a task with a waiting `update` (edit wins) or for a held task | Unchanged (the last copy the device saw); the tombstone rev is recorded separately. |
| Tombstone pulled, row removed (11.5) | Row deleted. |
| Unlink, relink | Unchanged. |
| Epoch change (10) | Snapshot kept, `trusted` set to 0; replaced by S during 9.2. A distrusted rev is never used as a `base_rev`. |

**Operation `base_rev` and collisions.** The base snapshot and an operation's `base_rev` are different things. When a new server copy S replaces the base B, a waiting `pending` `update` is *collision-free* if, for every group g in its `changed` (excluding groups the acknowledged operation itself changed), S(g) = B(g) or S(g) equals the local value. Then its `base_rev` moves to S's rev (safe: the server value of g is what the user saw). If some g has S(g) ≠ B(g) and S(g) ≠ local(g), another device changed a group the user also edited, and the user has not seen S(g): the row **keeps** its `base_rev` (for a row that waited behind an in-flight one, its `authored_rev`). The local task keeps the user's value, and the server later applies 6.2: the later arrival wins and the overwritten value is stored as a server conflict record and delivered to the device that lost it. Moving `base_rev` past such a change would overwrite it without any record; this rule exists to prevent that. In-flight rows are never changed; the server arbitrates them the same way.

**Sequences.** (1) Task at rev 3. The user edits the title (U1) and U1 is sent; the user edits the priority (U2) and the notes (U3). Base: rev 3 throughout; U1 in flight, U2 and U3 waiting with `after: U1` and `authored_rev` 3. (2) U1 acknowledged at rev 4: base = rev 4 snapshot, which has U1's title and no priority or notes edit; U2 and U3 get `base_rev` 4; local = base plus their groups. (3) Instead, the response is lost, the server had applied U1, and a pull delivers rev 4 first: base = rev 4, U1 stays in flight, the replayed result later has an equal rev and changes nothing. (4) Instead, U1 is rejected `validation_failed`: base stays rev 3, U1 and its dependents are dropped, the task is `not_synced`, and the next edit computes `changed` as the groups where the task differs from the rev 3 base. (5) The app is killed at any point: see 11.11.

### 11.11 Atomic acknowledgement and crash recovery

**One transaction per response.** When a push returns `200`, the client first checks, without writing:

1. The response body is complete and well formed; `results` has exactly one entry per operation of the batch, with the same `op_id`s in the same order. Otherwise the outcome is **unknown** (4.4) and nothing is applied.
2. `server_epoch` equals the stored epoch. Otherwise nothing is applied and section 10 starts.
3. The link generation (a counter in `sync_state`, bumped on unlink and sign-in) equals the one the request was started under, so a response that arrives after unlink or an account switch is dropped. This is re-read **inside** the transaction below.

Then it runs `BEGIN IMMEDIATE`, and in that single SQLite transaction: applies every result as described in 11.7 (task row, `server_rev`, base snapshot, dependent rows, `held` and `not_synced` flags, local conflict records from `conflicts`), deletes each acknowledged outbox row, and `COMMIT`s. The pull cursor is not touched. If any statement fails, or any result cannot be applied, the whole transaction is rolled back and **no** result of the response is kept; the rows stay `in_flight`, the error is shown ("Needs attention" for a persistent one), and the next attempt re-sends the batch byte-identical and receives the stored results with `replayed: true` (4.4). Results are never applied one by one in separate transactions. No network access and no UI signal happens inside the transaction; notices, `tasksMutated` and reminder rescheduling are emitted **after** the commit, and notices are persisted in the conflict table so they survive a crash between commit and display.

**Marking rows.** Marking a batch `in_flight` and storing each `sent_payload` is also one transaction (11.7) and happens before the request.

**Durability.** `synchronous` is never turned off. A commit that is lost on power failure equals a crash before the commit, which replay repairs; a partial commit cannot happen. Whether the journal is rollback or WAL does not change the rules.

**Idempotent application.** Applying a result depends only on the result and the local state, and the stored result is identical on every replay, so repeating the transaction after a rollback reaches the same state as if no crash had happened. A result whose `op_id` has no `in_flight` row (a duplicate delivery, or a batch already acknowledged) is ignored.

| Crash point | State after restart | Recovery |
|---|---|---|
| Before marking rows `in_flight` | Rows `pending`, nothing sent | Normal batch build. |
| After marking, before or while sending | Rows `in_flight`, outcome unknown | Next batch begins with them, byte-identical (11.7). |
| After the server committed, before the response arrived or was read | Same as above; the server holds the result | Replay returns `replayed: true` results; applied by the transaction above. |
| Response received, before `BEGIN` | Nothing written | Same. |
| Inside the transaction, before `COMMIT` (any statement) | SQLite rolls the transaction back on the next open: no revision, base, flag, conflict record or row deletion survives | Same: replay, then apply. |
| After `COMMIT` | Complete: revisions and bases updated, rows deleted, records stored | Nothing to repeat. Notices are shown from the stored records; derived state (UI, reminders) is rebuilt from the database at startup. |
| After `COMMIT`, a duplicate of the same response arrives | Rows are gone | The results are ignored. |
| A pull page applied (5.2) | Items and cursor are in one transaction | Same rules: all or nothing. |

A pull that completes between a crash and the replay raises the stored rev; the replayed older result then leaves the base alone (11.10).

### 11.12 Local conflict records

**Table `sync_conflicts` (logical schema).**

| Column | Meaning |
|---|---|
| `id` | Local primary key. |
| `record_uid` | The server's conflict id for records delivered by the server (null for locally detected ones). |
| `source` | `local` (detected by this device) or `server` (delivered in a push result or pull). |
| `kind` | `reconcile` (9.2 and 11.6, both sides changed or no base), or the server kinds `field`, `setting`, `edit_over_delete`, `delete_lost_to_edit` (6.3). |
| `task_uuid`, `setting_key` | The task or setting concerned (one is null). |
| `field_group` | One of the groups in 2.2 (null for the two delete kinds). |
| `mine_value` | The **full group value this device held and did not keep** (a JSON scalar, or `{completed, completed_at}` for `completion`). For a server record where this device lost: the server's `losing_value`. Never replaced or edited after creation. |
| `theirs_value` | The value that was kept (the server's `winning_value`, or S(g)). |
| `base_value` | The device's base value of the group at detection, null when no base was known. |
| `server_rev_seen` | The rev of the server copy at detection (`change_seq` for server records). It identifies *which* cloud value `theirs_value` is. |
| `other_device`, `detected_at`, `change_seq` | Context for the notice; times are informational. |
| `state` | `open`, `kept_cloud` or `restored`. |
| `restore_op_id` | The outbox row created by a restore (null otherwise). |
| `resolved_at` | When `state` last left `open` (local clock; used only by cleanup). |
| `notified` | 0 until the notice was shown. |

**Uniqueness.** `record_uid` is unique when present. For local records `(kind, task_uuid, setting_key, field_group, server_rev_seen)` is unique. Insertion is insert-or-ignore, so a repeated reconciliation or a repeated delivery never duplicates a record and never resets its `state`.

**Preserving values.** `mine_value`, `theirs_value` and `base_value` are written, with complete values (the whole `notes` text included), **in the same transaction** that replaces the local value (9.2, 11.6) or stores the server record (11.7, 11.5), so there is no moment at which a replaced local value exists nowhere. Records are written for groups only when the two values differ.

**Resolution.** A record is never resolved automatically.

- **Keep cloud value** (also "Dismiss" for notice-only kinds): `state` = `kept_cloud`, `resolved_at` set. No operation, no change to the task. The preserved value stays available.
- **Restore my value** (`field`, `reconcile`; `setting` is the same with `set_setting` and the setting's rev): a new deliberate user edit that goes through the normal write path (11.2) as an `update` for that group with `restore_of` = the record id, the value `mine_value`, and `base_rev` = the task's **current** base rev. It is never based on `server_rev_seen`. It is queued only if all of these hold: (G1) the task exists locally, is not held and not deleted, otherwise the result is `unavailable` and nothing changes; (G2) the group's local value, its base value and `theirs_value` are all equal, meaning the device still shows exactly the cloud value the conflict resolved to, with no newer local edit and no newer cloud value; otherwise the result is `needs_confirm`, nothing is queued, and the UI shows the current value next to `mine_value`; (G3) a confirmed restore after `needs_confirm` is queued the same way, so the user has seen and accepted the current value. On queueing, `state` = `restored`, `restore_op_id` and `resolved_at` are set, in the same transaction as the row.
- **Restore rows are strict.** If, before the row is sent, a pull or acknowledgement brings a different cloud value for that group (a collision, 11.10), the group is dropped from the row (the row is deleted when empty), the record returns to `open` with `theirs_value` and `server_rev_seen` updated to the new cloud value, and a notice says so. Newer cloud data is never overwritten by a restore that was decided against older data. If the row is rejected, or dropped for any other reason, the record returns to `open`.
- **Residual race.** Once a restore row is `in_flight`, another device may still change the group before the server applies it. The server then applies 6.2: the restore wins by arrival, and the value it overwrote is stored as a server conflict record and delivered to the other device. It is recorded, never silent.
- **Delete record**: only on explicit user confirmation; removes the row and its preserved values.

**No silent overwrite.** Nothing but the user's explicit Restore or Delete changes or removes `mine_value`. No path writes `mine_value` into the task without the guards above, and no restore is based on a rev the device has not adopted.

**Retention and cleanup.** `open` records are **never** deleted automatically, whatever their age, because they may hold the only copy of the user's text; this includes records of tasks that no longer exist locally, records kept across unlink, relink and epoch changes (they stay, listed as belonging to the old link). `kept_cloud` and `restored` records are deleted 30 days after `resolved_at`. Cleanup runs at startup and after each successful sync, never on a timer, and deletes only such rows. Server records are kept by the server for 90 days (6.3); local retention does not depend on that. The UI must offer a list of open records, with a bulk "Keep cloud value" action that needs a confirmation stating the count.

## 12. Limits and errors

Advertised by `GET /v1/version`: 200 operations per push, 1 MiB per request body, 500 changes per pull page. Errors use one envelope, `{ "error": { "code", "message", "request_id" } }`, never echo user content, and rate limits answer 429 with `Retry-After`. Request logs MUST NOT contain task content.

## 13. Versioning and compatibility

The protocol integer is 1. Clients send `X-TaskNest-Protocol: 1`; `GET /v1/version` returns the supported range. Within `/v1` fields are only added, never renamed or removed, and clients ignore fields they do not know. TaskNest never forces an update: if a client's protocol is no longer supported, sync stops with a clear message and everything else keeps working locally.
