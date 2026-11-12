# Build and test

Source: `.claude/CLAUDE.md` (Build, Testing and Reporting) and `CMakeLists.txt`, `tests/CMakeLists.txt`.

## Build (Windows, MinGW + Ninja, Qt 6.12.0)

PATH needs `D:\Apps\Qt6\Tools\mingw1310_64\bin`, `D:\Apps\Qt6\Tools\Ninja`,
`D:\Apps\Qt6\Tools\CMake_64\bin`, `D:\Apps\Qt6\6.12.0\mingw_64\bin`. From the repo root:

```
cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH=D:/Apps/Qt6/6.12.0/mingw_64 -DCMAKE_CXX_COMPILER=D:/Apps/Qt6/Tools/mingw1310_64/bin/g++.exe
cmake --build out/build
ctest --test-dir out/build --output-on-failure
```

- Build output: `out/bin/TaskNest.exe`, `out/bin/TaskNestHost.exe`, runtime in `out/bin/runtime/` (windeployqt, disposable).
- Clean with `cmake --build out/build --target clean`. Never delete `out/bin/data/` (live data).
- The C++ build and CTest must not need Python, Docker or PostgreSQL.

## Tests (`tests/`)

| CTest name | Executable | Notes |
|---|---|---|
| `tasknest` | `tst_tasknest` | Main suite; starts `tst_lockowner`, `TaskNest.exe --test-profile` and `TaskNestHost` as disposable processes. |
| `tasknest_sync` | `tst_sync` | Sync groundwork against temporary SQLite. |
| `tasknest_ui` | `tst_ui` (`ui_main.cpp`) | QML UI tests in `tests/qml/` (`tst_ui.qml`, `tst_datetime.qml`, `tst_extension.qml`, failure variants). |
| `qt_test_wrapper` | `tst_fakeqt` | Self-test of `RunQtTest.cmake`. |

- `RunQtTest.cmake` writes `out/build/tests/*.log`, prints `SUMMARY: N passed, N failed, N skipped`,
  and marks a run INCOMPLETE when skips exceed `TASKNEST_SKIP_BUDGET` / `TASKNEST_UI_SKIP_BUDGET` (default 0).
- Tests use temporary databases, never `out/bin/data/`.
- `tests/e2e/run_e2e.py` exists but its role is not verified in this memory; read it before use.

## Registration rules

- New `.cpp`: add to `qt_add_library(tasknestcore ...)` in `CMakeLists.txt`, with its header in `scripts/include/`.
- New QML component: add to `scripts/main/qmldir` **and** `QML_FILES` in `CMakeLists.txt`.
- New test file: add to the matching `tests/CMakeLists.txt` target; keep `objectName`s used by UI tests.

## Server (separate, off by default)

Python steps live in [cloud-and-server.md](cloud-and-server.md). They never run as part of the C++ build.
