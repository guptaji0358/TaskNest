# TaskNest local integration contract (protocol 1)

TaskNest can accept a small set of requests from other programs running **as
the same Windows user on the same machine**. It is off by default, local only,
and never starts TaskNest for you.

## Enabling and disabling

* **Settings → Shortcuts, startup & motion → Local integration API** turns the
  endpoint on or off immediately. The choice is stored in the database
  (`app_metadata`, key `integration_enabled`) and is restored at the next start.
* While it is off there is no pipe, no token and no discovery file.
* The endpoint exists only while TaskNest is running. Closing TaskNest closes
  the pipe and deletes the discovery file. There is no service and no
  background process.

## Transport

* A Windows **named pipe** created with Qt `QLocalServer`, restricted to the
  current user (`UserAccessOption`). Pipe name: `\\.\pipe\TaskNest-<16 hex>`
  (derived from the user name and the database path, so two databases do not
  collide).
* **One request per connection:** the client connects, writes one UTF-8 JSON
  object terminated by `\n`, reads one JSON line back, and the server closes
  the connection. The server keeps an answered connection open for up to 3 s so
  the client can read the answer (closing at once could discard an answer the
  client had not read yet) and ends it as soon as the client closes.
* Limits: a request line may be at most **16 384 bytes**; at most 16
  simultaneous connections; a connection that has not delivered a complete
  line within **5 seconds** is closed with `timeout`.

## Discovery and authorization

While enabled, TaskNest writes a discovery file:

```
%LOCALAPPDATA%\TaskNest\integration\integration-<16 hex>.json
{"protocol":1,"pipe":"TaskNest-ab12...","token":"<64 hex chars>","pid":1234}
```

* `token` is 256 random bits generated **every time the endpoint starts**.
  Every request must carry it; a request without it, or with a stale one from
  an earlier run, is refused with `unauthorized` before anything else is
  examined. Comparison is constant-time.
* The file is created readable/writable by its owner only, inside the user's
  own profile.

### How a client detects availability

1. Read the newest `integration-*.json`. Missing file → TaskNest is not
   running, or the integration is off.
2. Connect to `\\.\pipe\<pipe>`. Failure → the file is stale (TaskNest was
   killed); treat as unavailable.
3. Optionally send `ping`. `ok:true` means ready.

Clients must report "unavailable" to the user. They must **not** launch
TaskNest to deliver a request.

## Request and reply

```json
{"v":1,"token":"<token>","op":"create_task","args":{"title":"Buy milk","notes":"2 litres"}}
```

* Allowed top-level keys: `v` (must be `1`), `token`, `op`, `args` (object,
  optional). Any other key → `unknown_field`.
* Every operation rejects unsupported argument names with `unknown_field`
  instead of ignoring them.

Success: `{"ok":true,"result":{...}}`  
Failure: `{"ok":false,"error":{"code":"...","message":"..."}}`

## Operations

| `op` | `args` | Result | Effect |
|---|---|---|---|
| `ping` | none | `{"app":"TaskNest","protocol":1}` | none; availability check |
| `create_task` | `title` (required, ≤ 500 chars, one line), `notes` (optional, ≤ 10 000) | `{"task_id": n}` | creates a task in Inbox through the normal task model (reminders, widget views and cleanup rules behave as for any task) |
| `capture_page` | `url` (required, http/https, ≤ 2048), `title` (optional, ≤ 500), `notes` (optional) | `{"task_id": n}` | creates a task titled with the page title (or `host/path`). The address is stored as a final `Source: <url>` line in the notes — there is no separate URL column and no schema change |
| `open_view` | `view`: `all` \| `today` \| `upcoming` \| `completed` \| `inbox` | `{"view": "..."}` | raises the main window and shows that view |
| `open_task` | `task_id` (positive integer) | `{"task_id": n}` | raises the main window and opens the task editor; `not_found` if it does not exist |
| `quick_add` | none | `{}` | raises the main window and opens Quick Add |

`open_*` and `quick_add` only ever show the **main window**. They never open
the floating widget. Windows may restrict a background process from taking
focus; the taskbar button then flashes instead.

Repeated identical `create_task` / `capture_page` requests create separate
tasks. There is no de-duplication.

## Error codes

| Code | Meaning |
|---|---|
| `bad_request` | not one JSON object, wrong field types, missing `op` |
| `unauthorized` | token missing or wrong |
| `unsupported_version` | `v` is not `1` |
| `unknown_operation` | `op` is not in the table above |
| `unknown_field` | an unsupported top-level key or argument name |
| `invalid_argument` | value missing, wrong type, empty, too long, contains control characters, bad URL scheme, bad id |
| `not_found` | `open_task` for an id that does not exist |
| `too_large` | request line over 16 384 bytes |
| `timeout` | no complete line within 5 s |
| `busy` | too many simultaneous connections |
| `internal_error` | the database refused the operation; `message` has the reason |
| `unavailable` | **client side only**: no endpoint to talk to |

## Threat model

* **In scope:** other users on the machine, other machines on the network, a
  web page trying to reach TaskNest, a local program trying to guess its way
  in, malformed or oversized input.
* **Why a named pipe:** it is not reachable over TCP/IP, has no port to scan
  and cannot be hit from a browser page. It is created for the current user
  only (other accounts are refused by the pipe's access control), and the
  secret needed to talk to it is only in that user's profile.
* **Not in scope:** a malicious program already running as the same user. It
  can read the same discovery file, and could equally open the database
  directly. The token separates *intended* clients from accidental or
  cross-user access; it is not a defence against code running with the user's
  own rights.
* **Surface kept small:** five operations, no file-system, shell or SQL access;
  every field is type-, length- and character-checked; nothing is executed or
  interpreted; page URLs are stored as text and never fetched.
* **Not provided:** encryption (the pipe never leaves the machine), rate
  limiting beyond the connection cap and timeout, or request signing.

## Examples

PowerShell:

```powershell
$f = Get-ChildItem "$env:LOCALAPPDATA\TaskNest\integration\integration-*.json" |
     Sort-Object LastWriteTime -Descending | Select-Object -First 1
$info = Get-Content $f.FullName | ConvertFrom-Json
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $info.pipe, "InOut")
$pipe.Connect(3000)
$w = New-Object System.IO.StreamWriter($pipe); $r = New-Object System.IO.StreamReader($pipe)
$w.Write((@{v=1; token=$info.token; op="create_task"; args=@{title="From a script"}} |
          ConvertTo-Json -Compress) + "`n"); $w.Flush()
$r.ReadLine()      # {"ok":true,"result":{"task_id":42}}
```

Reference implementation: `IntegrationClient` in
`scripts/include/integrationserver.h` / `scripts/src/integrationserver.cpp`.

## Deep links (`tasknest://`)

Deep links are **independent of the integration API**: they work whether or not
the API is enabled, carry no token, and are validated like any untrusted input.

| Link | Effect |
|---|---|
| `tasknest://task/<id>` | raises the main window and opens that task (id: 1–15 digits, no leading zero). A deleted or unknown id shows "That task no longer exists." |
| `tasknest://view/<name>` | raises the main window on `all`, `today`, `upcoming`, `completed` or `inbox` |
| `tasknest://quickadd` | opens the compact Quick Add window (the main window and the widget are not shown) |

Anything else — other hosts/actions, a query string, a fragment, user info, a
port, extra path segments, control characters, more than 256 characters — is
refused with the notice "That TaskNest link can't be opened." The notice never
repeats the link. Links never contain task titles, notes or secrets; there is
no link that creates, edits or deletes a task.

**Windows registration.** Under **Settings → Open tasknest:// links** TaskNest
writes `HKEY_CURRENT_USER\Software\Classes\tasknest` with the open command
`"<TaskNest.exe>" "%1"` (no administrator rights; removing the switch removes the
key). It is off until the user turns it on. Nothing is registered by
installing or starting TaskNest.

**Delivery to a running instance.** Windows starts a *new* `TaskNest.exe` with
the link as its argument. That process does not open the database: it finds the
running instance through the existing single-instance lock, allows it to take
the foreground, sends the link as a `WM_COPYDATA` message (tag `TNKL`) to its
main window and exits with code 0. The running instance validates the link
before acting. If no instance is running, the new process simply starts
normally and handles the link once its window exists. No hidden process is ever
started for a link.

The `WM_COPYDATA` channel is the same trust level as launching the executable
with that argument (any program of the same user may do either); it is limited
to the three link forms above.

## Global Quick Add shortcut

* **Settings → Global Quick Add shortcut**, off by default. The chosen
  combination is listed (Ctrl+Alt+N, Ctrl+Alt+T, Ctrl+Alt+Q, Ctrl+Alt+Space,
  Ctrl+Shift+Space) and shown as "Active: ..." once Windows accepted it.
  Stored in the database (`global_quick_add`).
* Registered with the Windows `RegisterHotKey` call: no keyboard hook, no key
  logging, no clipboard access. The registration lives only while TaskNest is
  running and is released when the shortcut is turned off or the app exits.
* Rules for a combination: at least two modifiers, one of them Ctrl or Alt; the
  Windows key is not allowed; the last key is a letter, digit, F1–F12 or Space.
* If another program already owns the combination, TaskNest shows "... is
  already used by another program. Choose a different shortcut." and stays
  usable; the choice is kept so another combination can be picked.
* Pressing it opens the compact Quick Add window only (title, optional details
  from the normal Quick Add panel, Enter to add, Esc to cancel). Pressing it
  again while the window is open just refocuses it. Closing the window returns
  the keyboard to the application that had it.
* It does not turn the integration API on, and the API does not depend on it.

## `request_id` (capture_page idempotency)

`capture_page` accepts an optional `request_id` (8 to 64 characters of
`A-Z a-z 0-9 _ -`). A second `capture_page` carrying the **same** id within 10
minutes of the first returns the first task (`"duplicate": true`) instead of
creating another. The last 128 ids are remembered, in memory only, and
forgotten when the endpoint stops. Requests without an id, or with different
ids, are never merged: there is no global de-duplication, so capturing the
same page twice on purpose creates two tasks.

## Browser extension and the native host

The Chrome/Edge extension in `browser-extension/` cannot open a named pipe, so
`TaskNestHost.exe` (Native Messaging host, `scripts/main/hostmain.cpp`,
`scripts/src/nativehost.cpp`) bridges it to this API. Contract:

* One framed message in (4-byte little-endian length + UTF-8 JSON, at most
  64 KiB), one framed reply out, then the process exits. A watchdog ends it
  after 12 seconds in any case.
* Only two operations exist:
  * `{"op":"status"}` -> `{"ok":true,"result":{"state":"ready"}}` (a `ping`).
  * `{"op":"capture","title"?,"url","text","request_id"?}` -> `capture_page`
    with `notes` = the text. Text is 1..10 000 characters, title 1..500,
    url at most 2048 (scheme and shape are validated by the API). Any other
    field, or an extension-supplied pipe, token, path or command, is rejected
    with `bad_message`.
* The pipe name and credential come only from the newest
  `%LOCALAPPDATA%\TaskNest\integration\integration-*.json`. The record is
  validated first (size <= 4 KiB, protocol 1, pipe `TaskNest-` + 16 lower-case
  hex digits, token 64 hex digits). The credential is used to build the
  request and is never part of any reply, log or error.
* Replies: `{"ok":true,"result":{...}}` or
  `{"ok":false,"error":{"code","message"}}` with codes `bad_message`,
  `too_large`, `unsupported_operation`, `invalid_argument`,
  `integration_disabled` (TaskNest.exe is running but there is no endpoint),
  `not_running`, `unauthorized`, `timeout`, `outcome_unknown`, `rejected`.
* **Confirmed failure vs unknown outcome (`capture` only).** Anything refused
  before the request was written (`not_running`, `integration_disabled`,
  validation, `unauthorized`, `rejected`) means nothing was stored. If the
  request *was* written and no complete answer came back (timeout, pipe
  closed), the host cannot know whether TaskNest stored it and answers
  `outcome_unknown`. Resending the **same `request_id`** is always safe: TaskNest
  returns the original task (`duplicate: true`) instead of creating another.
  A new `request_id` is a new capture and creates a new task. `status` has no
  side effects, so a missing answer there is a plain `timeout`.
* Replies carry `timing_ms` (`connect`, `answer`: numbers only) and, on
  failure, `client_code` (the client's own fixed vocabulary) for diagnosis.
* The host never starts TaskNest.
* Runtime libraries: a browser starts the host with its own environment. When
  Browser capture is switched on, TaskNest writes a launcher script next to the
  manifest that prepends the folders of the Qt/MinGW DLLs it is itself running
  from (only folders that exist and contain no characters the command
  processor treats specially) and then starts `TaskNestHost.exe` with the
  browser's arguments. The manifest `path` is that launcher. Nothing is
  bundled or copied; without the launcher the host exits with `0xC0000135`
  (DLL not found) when Qt is not on PATH.
* Reading the registration state never creates registry keys (read-only
  access); switching it off deletes TaskNest's own key under each browser and
  the manifest and launcher, nothing else.
* Registration (`NativeHostRegistration`): a manifest in
  `%LOCALAPPDATA%\TaskNest\native-host\` plus `HKCU` keys for Chrome and Edge,
  with `allowed_origins` containing exactly
  `chrome-extension://fghdblbielgdmgnmodfhdifeiggaglcj/` (derived from the
  `key` in the extension manifest; a test checks they agree).

## Capture selected text from other applications

Optional, off by default: **Settings -> Capture selection from other apps**,
shortcut chosen from a fixed list (default Ctrl+Alt+C), registered with
`RegisterHotKey` like the Quick Add shortcut. When pressed, TaskNest asks
Windows UI Automation (once, on a worker thread) for the text selection of the
focused element of the foreground application and shows it in a review window
with an editable title and the source (window title and executable name). A
task is created only when you press **Capture**; Esc or Cancel discards it and
returns the keyboard to the other application.

Limits, stated plainly:

* Works only with applications that expose their selection through UI
  Automation's TextPattern (for example WPF/WinUI text boxes, Word, many
  editors). Classic Win32/WinForms edit controls, many games, some custom-drawn
  editors, browsers without accessibility enabled and *elevated* windows
  cannot be read. In those cases the window says so and offers a Paste button
  that inserts the current clipboard when **you** press it.
* Password fields are detected (`IsPassword`) and never read.
* Nothing is sent as keystrokes, the clipboard is never read or written
  automatically, and there is no hook or polling, so there is no clipboard
  state to restore.
* Selections over 10 000 characters are truncated in the review window (and
  it says so).
* It is independent of the browser extension and of the integration API.

## Scratch test profile (`--test-profile`)

For acceptance testing only. **Never point it at the real data folder** (`out/bin/data`, `%APPDATA%\TaskNest`, `%LOCALAPPDATA%\TaskNest`); such folders are refused.

Normal launches are unchanged: the profile is selected only by the command-line option, never by an environment variable, a setting or a browser message.

```
TaskNest.exe --test-profile C:\scratch\tn-profile-1
```

| What | Production | With `--test-profile <root>` |
|---|---|---|
| Database + settings | `Database::defaultDatabasePath()` | `<root>\data\tasknest.db` |
| Instance lock | `<database>.lock` | beside that database |
| Integration discovery + pipe | `%LOCALAPPDATA%\TaskNest\integration`; pipe derived from user + database path | `<root>\integration`; the pipe differs because the database path differs |
| Host manifest + launcher | `%LOCALAPPDATA%\TaskNest\native-host` | `<root>\native-host` |
| Browser host registry keys | `HKCU\Software\Google\Chrome\NativeMessagingHosts\com.tasknest.host` and the Edge equivalent | `HKCU\Software\TaskNestTestProfiles\<16 hex of root>\...` (same relative layout) |
| `tasknest://` registration, Start Menu shortcut | real HKCU / Start Menu | refused |

Rules enforced by `TestProfile` (`scripts/include/testprofile.h`):

* `<root>` must be an absolute local drive path with no `.` or `..` segments, not a drive root, not UNC, and must not equal, contain or sit inside the exe folder, `<exe>\data`, `%APPDATA%\TaskNest` or `%LOCALAPPDATA%\TaskNest`.
* TaskNest creates the folder and a marker file `tasknest-test-profile.json`; a non-empty folder without the marker is refused. The host never creates anything and requires the marker.
* An invalid option stops TaskNest with exit code 2 before any database or real location is touched.

Host: with a profile, the launcher TaskNest writes passes `--test-profile "<root>"` to `TaskNestHost.exe`. The host then uses only `<root>\integration` (no fallback to the per-user folder). A missing or unmarked profile, or an unexpected argument, answers every request with `configuration` and contacts nothing. Without the argument the host is the normal one and never looks in a profile. Authentication is unchanged and no token is ever on a command line.

**Limitation:** browsers read only the real `NativeMessagingHosts` keys, so a profile's registry subtree isolates TaskNest's own bookkeeping and the automated tests; it does **not** make Chrome or Edge discover the host. It is a test mechanism, not a way to install the extension.

Clean up: close the scratch TaskNest (turn browser capture off in its Settings first, or delete `HKCU\Software\TaskNestTestProfiles\<hash>`), then delete `<root>`. Do not delete anything else.

Instance identity: "is TaskNest running?" (`NativeHost::isInstanceRunning`) reads the single-instance lock `<database>.lock` of the database being served and requires its owner to be a live `TaskNest.exe`; it never matches by executable name alone, so a scratch instance cannot be mistaken for the normal one (or the reverse). A profile's host asks about `<root>\data\tasknest.db.lock`; the production host asks about `<exe folder>\data` and `%APPDATA%\TaskNest`. The profile folder creates `data\` up front so that this lock can be taken on the very first launch.

Verified by launching the real executable (see the Phase 4H report): the profile is created with its marker, database and lock, a second launch of the same profile exits with code 0 without a window, a second profile runs alongside it, and `TaskNestHost.exe --test-profile` reports `integration_disabled` / `not_running` / `ready` per profile.
