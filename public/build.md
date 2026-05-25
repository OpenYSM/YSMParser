# Build

This directory contains the Svelte/Tauri desktop frontend for YSMParser.
The Tauri app depends on the native C++ `YSMParser` executable as a sidecar.

## Local Desktop Build

Run from the repository root:

```bash
nix develop .#tauri --command bash -c '
  cmake -S . -B build-cpp -G Ninja -DCMAKE_BUILD_TYPE=Release -DYSM_TARGET_JNI=OFF &&
  cmake --build build-cpp --parallel &&
  mkdir -p public/src-tauri/binaries &&
  cp build-cpp/YSMParser/YSMParser public/src-tauri/binaries/YSMParser-x86_64-unknown-linux-gnu &&
  chmod +x public/src-tauri/binaries/YSMParser-x86_64-unknown-linux-gnu &&
  cd public &&
  pnpm install --frozen-lockfile &&
  pnpm tauri build --no-bundle
'
```

The desktop binary is written to:

```text
public/src-tauri/target/release/ysm-parser
```

## Cross-Build Windows Tauri From Linux

Tauri supports cross-building Windows NSIS installers from Linux/macOS with
caveats. This project also needs a Windows parser sidecar, so the local release
script cross-builds the C++ `YSMParser.exe` with MinGW before running Tauri
with `cargo-xwin`.

Run from the repository root:

```bash
scripts/release.sh tauri-windows-x64
```

The script enters `nix develop .#tauri-windows` automatically when `flake.nix`
is available. The first run can download the Windows SDK through `cargo-xwin`;
by default that cache is stored in:

```text
out/xwin-cache/
```

The generated archive is written under:

```text
out/release/
```

Cross-built Windows `.msi` installers are not supported by Tauri because WiX
only runs on Windows. The cross-build target intentionally produces an NSIS
setup `.exe`.

## macOS Tauri Builds

macOS `.app` and `.dmg` bundles must be produced on a macOS host. From a Mac,
run:

```bash
scripts/release.sh tauri
```

For automated release artifacts, use the macOS GitHub Actions runner configured
in `.github/workflows/ci.yml`.

## Browser-only Web Build

The browser build does not use the native Tauri sidecar. It needs the web
WebAssembly build of the parser next to the generated frontend files.

Run from the repository root:

```bash
nix develop .#wasm --command bash public/scripts/build-web.sh
```

Or from `public/` after entering the wasm shell:

```bash
pnpm run build:web
```

The static site is written to:

```text
public/dist/
```

Serve `public/dist/` with any static web server. Do not open `index.html`
directly from the filesystem, because browser WASM loading requires HTTP.

For a quick local preview:

```bash
nix develop .#wasm --command bash -c '
  cd public &&
  pnpm preview --host 127.0.0.1
'
```

## Flatpak Build

The Flatpak manifest is:

```text
public/flatpak/com.openysm.ysmparser.json
```

Run from the repository root:

```bash
nix develop --command bash -c '
  cd public &&
  flatpak-builder --disable-rofiles-fuse --force-clean build flatpak/com.openysm.ysmparser.json
'
```

To install the Flatpak locally:

```bash
nix develop --command bash -c '
  cd public &&
  flatpak-builder --user --install --force-clean build flatpak/com.openysm.ysmparser.json
'
```

Then run:

```bash
flatpak run com.openysm.ysmparser
```

## Notes

- The Flatpak manifest builds the C++ sidecar first, copies it into
  `public/src-tauri/binaries/YSMParser-x86_64-unknown-linux-gnu`, builds the
  Svelte frontend, then builds the Tauri shell.
- Flatpak builds run `pnpm` with `CI=true` so dependency installation works in
  the non-interactive builder environment.
- `public/build/`, `public/dist/`, `public/node_modules/`, and
  `public/.flatpak-builder/` are generated outputs.
