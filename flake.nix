{
  description = "Development environments for YSMParser";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { nixpkgs, ... }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
          };

          mkHook = lines: builtins.concatStringsSep "\n" (lines ++ [ "" ]);

          emsdkCompat = pkgs.runCommand "emsdk-compat-${pkgs.emscripten.version}" { } (
            "mkdir -p \"$out/upstream\"\n"
            + "ln -s ${pkgs.emscripten}/share/emscripten \"$out/upstream/emscripten\"\n"
          );

          nativePackages = with pkgs; [
            cmake
            ninja
            gcc
            jdk
            git
          ];

          nativeEnv = mkHook [
            "export JAVA_HOME=\"${pkgs.jdk.home}\""
            "export CMAKE_GENERATOR=\"Ninja\""
            "export CMAKE_PREFIX_PATH=\"${pkgs.jdk.home}:\${CMAKE_PREFIX_PATH:-}\""
            "export CMAKE_INCLUDE_PATH=\"${pkgs.jdk.home}/include:${pkgs.jdk.home}/include/linux:\${CMAKE_INCLUDE_PATH:-}\""
            "export CMAKE_LIBRARY_PATH=\"${pkgs.jdk.home}/lib:${pkgs.jdk.home}/lib/server:\${CMAKE_LIBRARY_PATH:-}\""
          ];

          emCacheEnv = mkHook [
            "export EM_CACHE=\"$HOME/.cache/emscripten/ysmparser\""
            "mkdir -p \"$EM_CACHE\""
          ];
        in
        rec {
          native = pkgs.mkShell {
            packages = nativePackages;

            shellHook = nativeEnv;
          };

          default = native;

          wasm = pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              emscripten
              nodejs_22
              pnpm
              git
            ];

            shellHook = nativeEnv + mkHook [
              "export EMSDK=\"${emsdkCompat}\""
            ] + emCacheEnv;
          };

          tauri = pkgs.mkShell {
            packages = nativePackages ++ (with pkgs; [
              rustc
              cargo
              rustfmt
              clippy
              rust-analyzer
              cargo-tauri
              nodejs_22
              pnpm
              pkg-config
            ]);

            buildInputs = with pkgs; [
              openssl
              glib
              gtk3
              webkitgtk_4_1
              libsoup_3
              librsvg
              dbus
              libayatana-appindicator
            ];

            shellHook = nativeEnv + mkHook [
              "export RUST_SRC_PATH=\"${pkgs.rustPlatform.rustLibSrc}\""
              "export WEBKIT_DISABLE_DMABUF_RENDERER=1"
            ];
          };

          tauri-windows = pkgs.mkShell {
            packages = nativePackages ++ (with pkgs; [
              rustup
              cargo-xwin
              lld
              llvmPackages.llvm
              nsis
              nodejs_22
              pnpm
              pkg-config
              pkgsCross.mingwW64.stdenv.cc
            ]);

            shellHook = nativeEnv + mkHook [
              "export PATH=\"$HOME/.cargo/bin:$PATH\""
              "export XWIN_CACHE_DIR=\"$PWD/out/xwin-cache\""
            ];
          };
        });
    };
}
