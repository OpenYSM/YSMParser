#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"
script_path="$script_dir/$(basename -- "${BASH_SOURCE[0]}")"

cd "$repo_root"

release_dir="${YSM_RELEASE_OUT:-$repo_root/out/release}"
internal_target=""
targets=()

usage() {
  cat <<'EOF'
Usage: scripts/release.sh [options] [target...]

Build local release artifacts without creating or uploading a GitHub release.
Tauri desktop bundles are built for the current host OS by default.

Targets:
  native      Build and package the host native CLI/JNI release.
  wasm-node   Build and package the Node.js WASM release.
  web         Build and package the browser app in public/dist.
  tauri       Build and collect the host Tauri desktop bundle.
  tauri-windows-x64
              Cross-build the Windows x64 Tauri NSIS installer from Linux/macOS.
  tauri-macos-arm64, tauri-macos-x64
              Build macOS Tauri bundles on a matching macOS host only.
  all         Run native, wasm-node, web, and tauri.

Default targets:
  native tauri

Options:
  --out DIR   Write artifacts under DIR. Default: out/release.
  --no-nix    Do not auto-enter the repository Nix dev shell.
  -h, --help  Show this help.

Environment:
  YSM_RELEASE_OUT                  Same as --out.
  YSM_RELEASE_NO_NIX=1             Same as --no-nix.
  YSM_RELEASE_TAURI_BUNDLES        Override Tauri bundle target.
                                    Defaults: deb on Linux, dmg on macOS, nsis on Windows.
                                    Use "none" to run tauri build --no-bundle.
  XWIN_CACHE_DIR                   cargo-xwin Windows SDK cache.
EOF
}

log() {
  printf '[release] %s\n' "$*"
}

die() {
  printf '[release] error: %s\n' "$*" >&2
  exit 1
}

is_windows() {
  case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

normalize_out_dir() {
  local path="$1"
  case "$path" in
    /*) printf '%s\n' "$path" ;;
    *) printf '%s/%s\n' "$repo_root" "$path" ;;
  esac
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --internal-target)
      [[ $# -ge 2 ]] || die "--internal-target requires a value"
      internal_target="$2"
      shift 2
      ;;
    --out)
      [[ $# -ge 2 ]] || die "--out requires a directory"
      release_dir="$(normalize_out_dir "$2")"
      shift 2
      ;;
    --no-nix)
      export YSM_RELEASE_NO_NIX=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      targets+=("$1")
      shift
      ;;
  esac
done

release_dir="$(normalize_out_dir "$release_dir")"
export YSM_RELEASE_OUT="$release_dir"

expand_targets() {
  local target
  for target in "$@"; do
    case "$target" in
      all)
        printf '%s\n' native wasm-node web tauri
        ;;
      wasm-web)
        printf '%s\n' web
        ;;
      native|wasm-node|web|tauri)
        printf '%s\n' "$target"
        ;;
      win|windows|windows-x64|tauri-windows-x64)
        printf '%s\n' tauri-windows-x64
        ;;
      macos|macos-arm64|tauri-macos-arm64)
        printf '%s\n' tauri-macos-arm64
        ;;
      macos-x64|tauri-macos-x64)
        printf '%s\n' tauri-macos-x64
        ;;
      *)
        die "unknown target: $target"
        ;;
    esac
  done
}

target_shell() {
  case "$1" in
    native) printf '%s\n' native ;;
    wasm-node|web) printf '%s\n' wasm ;;
    tauri) printf '%s\n' tauri ;;
    tauri-windows-x64) printf '%s\n' tauri-windows ;;
    tauri-macos-arm64|tauri-macos-x64) printf '%s\n' tauri ;;
    *) die "unknown target: $1" ;;
  esac
}

if [[ -n "$internal_target" ]]; then
  targets=("$internal_target")
elif [[ ${#targets[@]} -eq 0 ]]; then
  targets=(native tauri)
fi

mapfile -t targets < <(expand_targets "${targets[@]}")

if [[ -z "$internal_target" && "${YSM_RELEASE_NO_NIX:-}" != "1" && -f "$repo_root/flake.nix" && -x "$(command -v nix 2>/dev/null || true)" ]]; then
  for target in "${targets[@]}"; do
    shell_name="$(target_shell "$target")"
    log "running $target in nix develop .#$shell_name"
    nix develop ".#$shell_name" --command bash "$script_path" --internal-target "$target"
  done
  log "artifacts written under $release_dir"
  exit 0
fi

version="$(tr -d '\r\n' < "$repo_root/version.txt")"
[[ -n "$version" ]] || die "version.txt is empty"

prepare_staging_dir() {
  local name="$1"
  local dir="$release_dir/staging/$name"
  rm -rf "$dir"
  mkdir -p "$dir"
  printf '%s\n' "$dir"
}

copy_common_files() {
  local dir="$1"
  cp "$repo_root/LICENSE.txt" "$dir/"
  if [[ -f "$repo_root/README.md" ]]; then
    cp "$repo_root/README.md" "$dir/"
  fi
}

archive_dir() {
  local dir="$1"
  local stem="$2"
  mkdir -p "$release_dir"

  if is_windows; then
    local archive="$release_dir/$stem.zip"
    rm -f "$archive"
    if command -v zip >/dev/null 2>&1; then
      (cd "$dir" && zip -qr "$archive" .)
    elif command -v 7z >/dev/null 2>&1; then
      (cd "$dir" && 7z a -tzip "$archive" . >/dev/null)
    else
      local fallback="$release_dir/$stem.tar.gz"
      tar -C "$dir" -czf "$fallback" .
      log "zip/7z not found; wrote $fallback"
      return
    fi
    log "wrote $archive"
  else
    local archive="$release_dir/$stem.tar.gz"
    rm -f "$archive"
    tar -C "$dir" -czf "$archive" .
    log "wrote $archive"
  fi
}

set_native_vars() {
  local system
  local machine
  system="$(uname -s)"
  machine="$(uname -m)"

  case "$system" in
    Linux)
      if [[ "$machine" == "aarch64" || "$machine" == "arm64" ]]; then
        native_preset="linux-arm64-release"
        native_platform="linux-arm64"
      else
        native_preset="linux-release"
        native_platform="linux-x64"
      fi
      native_binary="out/install/$native_preset/YSMParser"
      native_jni="out/install/$native_preset/libYSMParserJNI.so"
      ;;
    Darwin)
      native_preset="macos-release"
      if [[ "$machine" == "arm64" || "$machine" == "aarch64" ]]; then
        native_platform="macos-arm64"
      else
        native_platform="macos-x64"
      fi
      native_binary="out/install/$native_preset/YSMParser"
      native_jni="out/install/$native_preset/libYSMParserJNI.dylib"
      ;;
    MINGW*|MSYS*|CYGWIN*)
      native_preset="x64-release"
      native_platform="windows-x64"
      native_binary="out/install/$native_preset/YSMParser.exe"
      native_jni="out/install/$native_preset/YSMParserJNI.dll"
      ;;
    *)
      die "unsupported native host: $system $machine"
      ;;
  esac
}

build_native() {
  set_native_vars
  local archive_name="YSMParser-$version-$native_platform"
  local staging

  log "building native preset $native_preset"
  cmake --preset "$native_preset"
  cmake --build --preset "$native_preset" --parallel
  cmake --install "out/build/$native_preset" --prefix "out/install/$native_preset"

  [[ -f "$repo_root/$native_binary" ]] || die "missing native binary: $native_binary"

  staging="$(prepare_staging_dir "$archive_name")"
  cp "$repo_root/$native_binary" "$staging/"
  if [[ -n "$native_jni" && -f "$repo_root/$native_jni" ]]; then
    cp "$repo_root/$native_jni" "$staging/"
  fi
  copy_common_files "$staging"
  archive_dir "$staging" "$archive_name"
}

build_wasm_node() {
  local preset="wasm-release"
  local archive_name="YSMParser-$version-wasm-node"
  local staging
  local wasm_file

  log "building $preset"
  cmake --preset "$preset"
  cmake --build --preset "$preset" --parallel
  cmake --install "out/build/$preset" --prefix "out/install/$preset"

  wasm_file="$(find "$repo_root/out/install/$preset" -maxdepth 1 -type f -name '*.wasm' | head -n 1)"
  [[ -f "$repo_root/out/install/$preset/YSMParser.js" ]] || die "missing out/install/$preset/YSMParser.js"
  [[ -n "$wasm_file" ]] || die "missing wasm file in out/install/$preset"

  staging="$(prepare_staging_dir "$archive_name")"
  cp "$repo_root/out/install/$preset/YSMParser.js" "$staging/"
  cp "$wasm_file" "$staging/"
  copy_common_files "$staging"
  archive_dir "$staging" "$archive_name"
}

build_web() {
  local archive_name="YSMParser-$version-web"
  local staging

  log "building browser app"
  "$repo_root/public/scripts/build-web.sh"

  [[ -d "$repo_root/public/dist" ]] || die "missing public/dist"
  staging="$(prepare_staging_dir "$archive_name")"
  cp -R "$repo_root/public/dist/." "$staging/"
  copy_common_files "$staging"
  archive_dir "$staging" "$archive_name"
}

set_tauri_vars() {
  local system
  local triple
  system="$(uname -s)"
  triple="$(rustc -vV | awk '/^host: / {print $2}')"
  [[ -n "$triple" ]] || die "could not determine Rust host triple"

  case "$system" in
    Linux)
      tauri_bundle="${YSM_RELEASE_TAURI_BUNDLES:-deb}"
      tauri_binary_name="ysm-parser"
      sidecar_ext=""
      ;;
    Darwin)
      tauri_bundle="${YSM_RELEASE_TAURI_BUNDLES:-dmg}"
      tauri_binary_name="ysm-parser"
      sidecar_ext=""
      ;;
    MINGW*|MSYS*|CYGWIN*)
      tauri_bundle="${YSM_RELEASE_TAURI_BUNDLES:-nsis}"
      tauri_binary_name="ysm-parser.exe"
      sidecar_ext=".exe"
      ;;
    *)
      die "unsupported Tauri host: $system"
      ;;
  esac

  tauri_triple="$triple"
  tauri_sidecar_name="YSMParser-$triple$sidecar_ext"
  tauri_sidecar_source="out/build/release-tauri-sidecar/YSMParser/YSMParser$sidecar_ext"
}

copy_tauri_outputs() {
  local staging="$1"
  local bundle="$2"

  case "$bundle" in
    none|no-bundle)
      [[ -f "$repo_root/public/src-tauri/target/release/$tauri_binary_name" ]] || die "missing Tauri binary"
      cp "$repo_root/public/src-tauri/target/release/$tauri_binary_name" "$staging/"
      ;;
    deb)
      find "$repo_root/public/src-tauri/target/release/bundle/deb" -maxdepth 1 -type f -name '*.deb' -exec cp {} "$staging/" \;
      ;;
    dmg)
      find "$repo_root/public/src-tauri/target/release/bundle/dmg" -maxdepth 1 -type f -name '*.dmg' -exec cp {} "$staging/" \;
      ;;
    nsis)
      find "$repo_root/public/src-tauri/target/release/bundle/nsis" -maxdepth 1 -type f -name '*.exe' -exec cp {} "$staging/" \;
      ;;
    *)
      find "$repo_root/public/src-tauri/target/release/bundle" -type f -exec cp {} "$staging/" \;
      ;;
  esac

  if ! find "$staging" -maxdepth 1 -type f | grep -q .; then
    die "no Tauri outputs copied for bundle target: $bundle"
  fi
}

build_tauri() {
  set_tauri_vars
  local archive_name="YSMParser-$version-tauri-$tauri_triple"
  local staging

  log "building Tauri sidecar for $tauri_triple"
  cmake -S "$repo_root" -B "$repo_root/out/build/release-tauri-sidecar" -G Ninja -DCMAKE_BUILD_TYPE=Release -DYSM_TARGET_JNI=OFF
  cmake --build "$repo_root/out/build/release-tauri-sidecar" --parallel

  [[ -f "$repo_root/$tauri_sidecar_source" ]] || die "missing sidecar binary: $tauri_sidecar_source"
  mkdir -p "$repo_root/public/src-tauri/binaries"
  cp "$repo_root/$tauri_sidecar_source" "$repo_root/public/src-tauri/binaries/$tauri_sidecar_name"
  if ! is_windows; then
    chmod +x "$repo_root/public/src-tauri/binaries/$tauri_sidecar_name"
  fi

  log "building Tauri bundle target: $tauri_bundle"
  (
    cd "$repo_root/public"
    CI=true pnpm install --frozen-lockfile
    if [[ "$tauri_bundle" == "none" || "$tauri_bundle" == "no-bundle" ]]; then
      pnpm tauri build --no-bundle
    else
      pnpm tauri build --bundles "$tauri_bundle"
    fi
  )

  staging="$(prepare_staging_dir "$archive_name")"
  copy_tauri_outputs "$staging" "$tauri_bundle"
  archive_dir "$staging" "$archive_name"
}

write_mingw_toolchain_file() {
  local toolchain_file="$1"
  local cc
  local cxx
  local windres

  cc="$(command -v x86_64-w64-mingw32-gcc || true)"
  cxx="$(command -v x86_64-w64-mingw32-g++ || true)"
  windres="$(command -v x86_64-w64-mingw32-windres || true)"

  [[ -n "$cc" ]] || die "missing x86_64-w64-mingw32-gcc"
  [[ -n "$cxx" ]] || die "missing x86_64-w64-mingw32-g++"

  cat > "$toolchain_file" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$cc")
set(CMAKE_CXX_COMPILER "$cxx")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

  if [[ -n "$windres" ]]; then
    printf 'set(CMAKE_RC_COMPILER "%s")\n' "$windres" >> "$toolchain_file"
  fi
}

ensure_windows_rust_toolchain() {
  command -v rustup >/dev/null 2>&1 || die "missing rustup"
  command -v cargo-xwin >/dev/null 2>&1 || die "missing cargo-xwin"
  command -v makensis >/dev/null 2>&1 || die "missing NSIS makensis"
  command -v llvm-rc >/dev/null 2>&1 || die "missing llvm-rc"

  export PATH="$HOME/.cargo/bin:$PATH"
  rustup toolchain install stable --profile minimal --target x86_64-pc-windows-msvc
}

copy_tauri_windows_outputs() {
  local staging="$1"
  local bundle_dir="$repo_root/public/src-tauri/target/x86_64-pc-windows-msvc/release/bundle/nsis"

  [[ -d "$bundle_dir" ]] || die "missing Windows NSIS bundle directory: $bundle_dir"
  find "$bundle_dir" -maxdepth 1 -type f -name '*.exe' -exec cp {} "$staging/" \;

  if ! find "$staging" -maxdepth 1 -type f -name '*.exe' | grep -q .; then
    die "no Windows NSIS installer copied from $bundle_dir"
  fi
}

build_tauri_windows_x64() {
  local target_triple="x86_64-pc-windows-msvc"
  local archive_name="YSMParser-$version-tauri-windows-x64"
  local build_dir="$repo_root/out/build/release-windows-x64-sidecar"
  local toolchain_file="$build_dir/mingw-toolchain.cmake"
  local sidecar_source="$build_dir/YSMParser/YSMParser.exe"
  local sidecar_dest="$repo_root/public/src-tauri/binaries/YSMParser-$target_triple.exe"
  local staging

  if is_windows; then
    log "Windows host detected; using the native Tauri Windows build path"
    build_tauri
    return
  fi

  ensure_windows_rust_toolchain

  log "cross-building Windows sidecar with MinGW"
  mkdir -p "$build_dir"
  write_mingw_toolchain_file "$toolchain_file"
  cmake -S "$repo_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DYSM_TARGET_JNI=OFF \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_file"
  cmake --build "$build_dir" --parallel

  [[ -f "$sidecar_source" ]] || die "missing Windows sidecar: $sidecar_source"
  mkdir -p "$repo_root/public/src-tauri/binaries"
  cp "$sidecar_source" "$sidecar_dest"

  log "cross-building Windows Tauri NSIS installer with cargo-xwin"
  (
    cd "$repo_root/public"
    CI=true pnpm install --frozen-lockfile
    RUSTUP_TOOLCHAIN=stable pnpm tauri build \
      --runner cargo-xwin \
      --target "$target_triple" \
      --bundles nsis
  )

  staging="$(prepare_staging_dir "$archive_name")"
  copy_tauri_windows_outputs "$staging"
  archive_dir "$staging" "$archive_name"
}

build_tauri_macos_target() {
  local requested="$1"
  local system
  local machine
  system="$(uname -s)"
  machine="$(uname -m)"

  if [[ "$system" != "Darwin" ]]; then
    die "$requested requires macOS. Tauri DMG/App bundles are built on a Mac host; use the macOS CI runner for this target."
  fi

  case "$requested:$machine" in
    tauri-macos-arm64:arm64|tauri-macos-x64:x86_64)
      build_tauri
      ;;
    *)
      die "$requested requires a matching macOS runner architecture; current machine is $machine"
      ;;
  esac
}

for target in "${targets[@]}"; do
  case "$target" in
    native) build_native ;;
    wasm-node) build_wasm_node ;;
    web) build_web ;;
    tauri) build_tauri ;;
    tauri-windows-x64) build_tauri_windows_x64 ;;
    tauri-macos-arm64|tauri-macos-x64) build_tauri_macos_target "$target" ;;
    *) die "unknown target after expansion: $target" ;;
  esac
done

log "artifacts written under $release_dir"
