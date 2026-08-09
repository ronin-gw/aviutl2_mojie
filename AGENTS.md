# mojie development guide

## Project purpose

mojie is an AviUtl2 (AviUtl ExEdit2) common plugin. It replaces configured strings in text with one image or an ordered image composition. AviUtl2 renders images under the application data `Font` directory through the `<&name>` text control sequence, so mojie translates matches to native text controls instead of implementing a separate renderer.

## Current baseline

- Language: C++17
- Build system: CMake 3.16+ with Visual Studio 2019/x64
- Plugin kind: common plugin (`.aux2`)
- SDK: `aviutl2/aviutl2_sdk_mirror`, pinned by `scripts/setup.ps1`
- Test runtime: `sandbox/AviUtl2` (local-only and gitignored)
- Current implementation: global/local image libraries, image compositions, configuration UI, replacement/inline syntax engine, managed Font cache, and a `mojieテキスト` alias backed by AviUtl2's standard Text engine
- Configuration: global `data/Plugin/mojie/config.json`; local project-sidecar `mojie.json`
- Release package: `Plugin/mojie/mojie.aux2` and `Alias/mojieテキスト.object`

## Commands

Run these from PowerShell at the repository root:

```powershell
./scripts/setup.ps1
./scripts/build.ps1
./scripts/run.ps1
./scripts/package.ps1
```

Use `./scripts/build.ps1 -Configuration Release` for a release build. The build script deploys `mojie.aux2` into the isolated test runtime.
`package.ps1` creates the GitHub Release / AviUtl2 catalog archive under `dist`. See `docs/releasing.md` for the release checklist.

## Working rules

- Do not commit `.deps`, `build`, `sandbox`, AviUtl2 binaries, SDK downloads, user data, or generated packages.
- Do not launch AviUtl2 or capture its windows unless the user explicitly requests runtime/UI verification.
- Keep the project buildable with the checked-in scripts; avoid requiring global PATH changes.
- Keep the plugin x64 and Unicode.
- Keep host exports, SDK callbacks, script-module callbacks, and Win32 window procedures behind `noexcept`/catch-all ABI boundaries.
- Do not write mojie configuration merely because AviUtl2 or a project was opened. Save only after an explicit user edit or a clearly defined Save As transfer.
- Treat global and local configuration files as user data. Preserve external edits, use unique same-directory temporary files for atomic replacement, and never delete managed-looking cache files without an ownership manifest.
- The current JSON format intentionally has no schema-version compatibility layer. Once a public release establishes a format, future incompatible changes need an explicit migration design instead of silently rewriting old data.
- Add pure unit tests for parsing/replacement rules before wiring them into AviUtl2 callbacks.
- Keep `README.md` focused on non-technical users. Put architecture, build, review, and release-maintenance notes here or under `docs/`.
- Use Japanese for user-facing plugin strings and documentation unless interoperability requires English.
