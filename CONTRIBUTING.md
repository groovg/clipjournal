# Contributing

Thanks for helping! clipjournal deliberately stays small: one source file, no third-party dependencies, an exe under 400 KB.

The bar for a new feature is "many people need it, and it costs few lines". Before you start, read the roadmap and non-goals in the [README](README.md#roadmap). For anything bigger than a bug fix, open an issue first so we can agree on the approach.

## Setup

- Windows 10 or 11
- Visual Studio 2022 or Build Tools, with the "Desktop development with C++" workload
- CMake 3.20 or newer

```
cmake -S . -B build
cmake --build build --config Release
```

You don't need a developer prompt: CMake finds MSVC on its own.

## Checks

- **Self-test.** Run `build\Release\clipjournal.exe --selftest`. Exit code 0 means pass; any other code is the line number of the failing `CHECK` in `SelfTest()`. If you add pure logic (parsing, formatting), add a `CHECK` for it there. CI runs the self-test on every push and PR.
- **End-to-end.** Run `powershell -File tests\e2e.ps1`. It needs Windows PowerShell 5.1 and an interactive desktop. Close any running clipjournal first. The test runs in portable mode next to the exe, so your real history is not touched, and it restores your clipboard text afterwards.

## Pull request checklist

- [ ] Release build has zero warnings (`/W4`)
- [ ] `--selftest` and `tests\e2e.ps1` pass
- [ ] You tried the change by hand. For UI changes, check dark and light themes and 100% and 150% scaling
- [ ] README updated if you added a setting, key or menu item
- [ ] No new dependencies

## Code style

Match the existing code: short functions, early returns, and comments only where something isn't obvious. Comments that start with `// ponytail:` mark deliberate shortcuts and say where each one stops being enough. Change one only when that limit is actually hit, and say so in the PR.

## Reporting bugs

Please include:
- your Windows version (`winver`) and display scaling
- your `hotkey` from config.ini
- the app you copied from or pasted into, and whether it runs as administrator

## Security and privacy

Clipboard history can contain passwords and other secrets. Report privacy problems privately via **Security → Report a vulnerability** on GitHub, not in a public issue. One example is a password manager's copies getting recorded.

By contributing, you agree that your work is licensed under the [MIT License](LICENSE).
