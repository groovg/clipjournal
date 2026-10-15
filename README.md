# clipjournal

A small clipboard history for Windows 10/11 that can replace Win+V. It is a single exe of under 400 KB with no dependencies and no installer.

It records text, links, images (as PNG) and copied files. History survives reboots, and each entry is a plain file you can browse.

## Build

```
cmake -S . -B build
cmake --build build --config Release
```

The exe is written to `build\Release\clipjournal.exe`. To check the parsers, run `clipjournal.exe --selftest`: exit code 0 means everything passed.

## Keys in the popup

| Key | Action |
|---|---|
| typing | search |
| ↑ ↓ PgUp PgDn | move the selection |
| Enter / click | paste into the previous window |
| Shift+Enter | copy only, don't paste |
| Tab / Shift+Tab | switch the filter: All, Pinned, Text, Links, Images, Files |
| Ctrl+P | pin or unpin. Pinned items never expire |
| Shift+Del | delete the entry |
| Esc | close |

The tray icon menu has: edit config, reload config, open data folder, start with Windows, clear history, exit.

## Config

The config file is `%LOCALAPPDATA%\clipjournal\config.ini`. It is created on first run, and every setting is commented.

| Setting | Meaning |
|---|---|
| `hotkey` | For example `Win+V`, `Ctrl+Shift+V`, `Alt+F1`. If another app already owns the combination, a keyboard hook takes it over. |
| `keep` / `keep_images` | How long unpinned items live: `30m`, `12h`, `7d`, `4w`; `0` means forever. |
| `max_items` | Cap on the number of unpinned items. |
| `ignore_apps` | Exe names whose copies are never recorded. |

**Portable mode:** put a `config.ini` next to the exe, and the config and history will live in that folder.

## Privacy

Copies are skipped when the app marks them as secret. Password managers do this with the clipboard formats `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory=0` and `Clipboard Viewer Ignore`. Copies from the apps listed in `ignore_apps` are skipped too.

History is stored unencrypted in the data folder.

## Storage

Entries are kept in `items\<unix-ms>_<hash>[_pin].<txt|png|files>`. If you copy the same content again, the existing entry moves to the top instead of being duplicated.

## Known limits

- Auto-paste does not work in windows running as administrator, because Windows (UIPI) blocks the simulated input. The item is still put on the clipboard, so you can press Ctrl+V yourself.
- Text keeps only its plain-text form. Rich formats such as HTML and RTF are not saved.
- Search only covers the first 4 KB of each entry.
