# Local integration protocol (summary)

Normative text: `INTEGRATION.md` (protocol 1). This file is a navigation summary; code is authoritative
(`scripts/src/integrationserver.cpp`, `integrationclient.cpp`, `deeplink.cpp`, `nativehost.cpp`,
`globalhotkey.cpp`, `selectioncapture.cpp`, `hostmain.cpp`).

## Local API

- Off by default (`integration_enabled`). While off: no pipe, no token, no discovery file.
- Transport: Windows named pipe via Qt `QLocalServer`, current-user access only. Name `\\.\pipe\TaskNest-<16 hex>`.
- Discovery file contains `protocol`, `pipe`, `token` (64 hex), `pid`. Clients check the pipe to detect a stale file.
- Request: one JSON object `{"v":1,"token":...,"op":...,"args":{...}}`. Unknown top-level keys or argument names → `unknown_field`.
- Operations (five only): `create_task`, `capture_page`, `open_view`, `open_task`, `quick_add`.
  `open_*` and `quick_add` only raise the main window. No file, shell or SQL access.
- Error codes include `bad_request`, `unknown_operation`, `unknown_field`, `not_found`, `internal_error`.
- `capture_page` takes an optional `request_id` (8–64 chars), idempotent for 10 minutes, kept in memory only.

## Deep links

- `tasknest://task/<id>`, `tasknest://view/<name>`, `tasknest://quickadd`. `DeepLink::parse` is the only interpreter.
- Links carry no content or token. Invalid links are refused with a notice that never echoes the link.
- Scheme registered under HKCU only when enabled in Settings (`UrlSchemeRegistration`).
- A second launch forwards the link via `WM_COPYDATA` and never opens the database.

## Browser extension and native host

- `browser-extension/` (MV3) cannot open the pipe; it talks to `TaskNestHost.exe` via Native Messaging.
- Host operations only: `status` (ping) and `capture`. Never returns the credential; never launches TaskNest.
- Extension id is fixed by the manifest `key`; must equal `NativeHost::kExtensionId` (tested).
- Registration only from Settings (HKCU, off by default). Registry reads use read-only `RegistryUtil`.
- Once a `capture` is written and no answer arrives, the outcome is `outcome_unknown`, not a plain failure.

## Global hotkey and selection capture

- `GlobalHotkey` (`RegisterHotKey`, off by default) opens only `QuickCaptureWindow.qml`.
- `SelectionCapture` reads the selection through UI Automation on explicit request only (off by default).
  Stores nothing until confirmed; no keystroke injection, clipboard access or password fields.

## Change rules

- Any change to the protocol updates `INTEGRATION.md` and its tests together.
- Tests use `setDiscoveryDirectory`, never the real `%LOCALAPPDATA%\TaskNest\integration`.
