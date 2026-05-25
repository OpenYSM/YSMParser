#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
public_dir="$(cd -- "$script_dir/.." && pwd)"
repo_root="$(cd -- "$public_dir/.." && pwd)"
wasm_build_dir="$repo_root/out/build/wasm-web-release/YSMParser"
dist_dir="$public_dir/dist"

cd "$repo_root"

cmake --preset wasm-web-release
cmake --build --preset wasm-web-release

cd "$public_dir"

if [ ! -x "$public_dir/node_modules/.bin/vite" ]; then
  pnpm install --frozen-lockfile
fi
"$public_dir/node_modules/.bin/vite" build

install -Dm644 "$wasm_build_dir/YSMParser.js" "$dist_dir/YSMParser.js"
install -Dm644 "$wasm_build_dir/YSMParser.wasm" "$dist_dir/YSMParser.wasm"

printf 'Browser build written to %s\n' "$dist_dir"
