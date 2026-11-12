# Conventions and decisions

Rules come from `.claude/CLAUDE.md` (Coding Standards, UI Design System, Development Rules). This file
collects the ones that most often matter when editing, plus the recorded decisions.

## C++ conventions

- C++17+. Qt idioms (parent-child ownership, signals/slots); RAII/smart pointers elsewhere; no raw `new`/`delete` outside Qt ownership.
- One class per header/source pair, matching names, `#pragma once`.
- Headers in `scripts/include/`, sources in `scripts/src/`; `main.cpp` only wires things together.
- Keep long-running work off the UI thread. Surface errors; never ignore them.
- Match surrounding style; comments only where they add meaning.

## QML and UI conventions

- All colors, sizes, radii, spacing and durations come from the `Theme.qml` singleton. Never hard-code.
- Reusable components are `TN`-prefixed (`TNButton`, `TNTextField`, `TNDialog`, `TNPopupSurface` for all popups) plus `Icon`, `TaskRow`, `NavItem`, `TaskCheck`, `Atmosphere`.
- Do not name a property `onX` where `X` is another property (it reads as a change handler); use e.g. `accentText`.
- Motion only from `Theme` tokens; reduce-motion zeroes durations; no looping animations; state changes immediately.
- Keep `objectName`s used by `tests/qml/`. Every top-level window must accept `onClosing`.
- Urgency is conveyed by icon and words as well as color.
- No business logic in QML.

## Data and safety rules

- Never overwrite or delete user data or database files without explicit authorization.
- Never commit `*.db`, `*.sqlite*`, build artifacts or executables. `out/` is git-ignored; `out/bin/data/` is live data.
- Do not keep the live DB in a cloud-synced folder; the dev checkout sits under OneDrive.
- No new third-party dependencies, PostgreSQL/Docker/extra Qt installs without approval.
- Secrets never in SQLite, settings, logs or command lines.

## Process rules

- Inspect existing code and docs before changing anything.
- Cloud work: one owner-approved milestone at a time.
- Do not commit or push unless explicitly requested.

## Recorded decisions (see sources for rationale)

| Decision | Source |
|---|---|
| Offline-first; no account, no cloud, no forced updates by default. | `.claude/CLAUDE.md` Product Principles |
| Qt 6.12.0 is the verified environment; minimum 6.8 unverified. | `.claude/CLAUDE.md` Technology |
| Reminders use a single-shot timer, not polling. | `.claude/CLAUDE.md` Product Principles |
| Optional Windows startup, off by default, HKCU Run value only. | `.claude/CLAUDE.md` Behavior Reference |
| Named pipe with current-user access for the local API; no TCP. | `INTEGRATION.md` Threat model |
| Cloud: Python/FastAPI, PostgreSQL, Argon2id, opaque tokens; provider-neutral. | `CLOUD.md` §2, §7, §8 |
| Server alone orders changes; client UUIDs identify tasks. | `CLOUD.md` §5, `api/SYNC.md` §1 |
| Completed-task cleanup default is Never. | `.claude/CLAUDE.md` Behavior Reference |

## Open items (not decided or not verified here)

- The optional Android target is planned (M9) and has no code.
- Whether `SyncStore` is wired into `TaskRepository` mutations end to end is unverified.
