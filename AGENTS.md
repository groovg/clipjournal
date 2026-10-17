# AGENTS.md

Instructions for coding agents working on clipjournal (Claude Code, Codex, Copilot, Cursor and others).

## What this is

A clipboard history manager for Windows. All the code is in one C++20 file, `clipjournal.cpp`: raw Win32 plus GDI+, no third-party dependencies, static CRT. The goal is small, fast and boring. Check the README's roadmap and non-goals before adding features.

## Build and check

| Step | Command | Notes |
|---|---|---|
| Build | `cmake -S . -B build && cmake --build build --config Release` | Must compile at `/W4` with zero warnings. |
| Unit checks | `build\Release\clipjournal.exe --selftest` | Exit code 0 = pass; anything else is the failing line in `SelfTest()`. New pure logic gets a `CHECK` there. |
| End-to-end | `powershell -File tests\e2e.ps1` | Uses the real clipboard and needs an interactive desktop. Kill any running `clipjournal.exe` first: it is single-instance. |

## Map of clipjournal.cpp

The file is laid out in this order:

1. **helpers:** UTF-8, parsing, FNV-1a
2. **storage:** item files, `Load`/`Prune`/`Add`
3. **clipboard:** `Capture`/`ToClipboard`
4. **popup:** owner-drawn listbox, search, keys
5. **hotkey:** `RegisterHotKey`, falling back to a low-level hook
6. **config & tray**
7. **`WndProc`, `SelfTest`, `wWinMain`**

## Invariants: don't break these

- **The storage format is the database:** `items/<unix-ms>_<16-hex FNV-1a>[_pin].<ext>`. `.txt` is UTF-8 text, `.png` is PNG bytes, and `.files` is UTF-8 paths joined with `\n`. Changing it needs a migration for existing users.
- **Own clipboard writes are skipped by sequence number.** Keep setting `ownSeq` right after `CloseClipboard()`.
- **Injected input is tagged.** Everything the app sends with `SendInput` carries `dwExtraInfo = kOwnInput`, and the keyboard hook ignores those events.
- **The low-level keyboard hook has its own thread** (`HookMain`). Never do slow work in `KbHook`: Windows silently removes slow hooks.
- **Never weaken the secret filters.** Captures must keep honoring `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory=0`, `Clipboard Viewer Ignore` and `ignore_apps`.
- **GDI+ objects (thumbnails) must be destroyed before `GdiplusShutdown`.**
- **Owner-drawn listbox rows are capped at 255 px.**
- **Header order matters.** `NOMINMAX` and the `namespace Gdiplus { using std::min; using std::max; }` shim must stay before `<gdiplus.h>`.

## Conventions

- Keep it a single file and add no dependencies: no SQLite, no UI frameworks.
- Match the style: short functions, global app state, early returns, comments only for what isn't obvious.
- `// ponytail:` comments mark deliberate shortcuts and say where each one stops being enough. Leave them alone unless that limit is hit.
- UI strings are in English. A new config key goes into `kDefaultConfig` (comments on their own lines, since INI has no inline comments) and into the README.
- Never add AI-attribution trailers to commits or PRs.

## Gotchas

- The exe's version resource comes from `-DVERSION=X.Y.Z`. CI passes the git tag; local builds get 0.0.0. In PowerShell, quote the whole argument (`"-DVERSION=$v"`), otherwise PowerShell splits it at the dot.
- In PowerShell tests, pass `[NullString]::Value`, not `$null`, to Win32 string parameters: `$null` becomes `""`.
- A `config.ini` next to the exe turns on portable mode, and the tests rely on it. Don't leave one in `build\Release` after manual runs.
- The keyboard hook path can be tested with `keybd_event`. Only events tagged `kOwnInput` are ignored, not every injected one.
