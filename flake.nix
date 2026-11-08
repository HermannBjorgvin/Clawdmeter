{
  description = "Clawdmeter: desk-side Claude Code usage monitor (desktop app + firmware dev shell)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      # GPUI's macOS build compiles Metal shaders with Xcode's toolchain,
      # which the Nix sandbox doesn't have; the release .dmg covers macOS.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Linked at build time (pkg-config) or found through the rpath.
      linkLibs =
        pkgs: with pkgs; [
          alsa-lib
          dbus
          fontconfig
          freetype
          libxkbcommon
          libx11
          libxcb
        ];
      # dlopen()ed by GPUI at runtime: Vulkan (blade) and the Wayland client.
      # The GPU drivers come from the system (hardware.graphics on NixOS).
      runtimeLibs =
        pkgs: with pkgs; [
          vulkan-loader
          wayland
        ];
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          inherit (pkgs) lib;
          cargoToml = lib.importTOML ./desktop/Cargo.toml;
        in
        rec {
          clawdmeter = pkgs.rustPlatform.buildRustPackage {
            pname = "clawdmeter";
            version = cargoToml.workspace.package.version;

            # build.rs reads Clawd's art from the firmware's generated header.
            src = lib.fileset.toSource {
              root = ./.;
              fileset = lib.fileset.unions [
                ./desktop/Cargo.toml
                ./desktop/Cargo.lock
                ./desktop/crates
                ./desktop/vendor
                ./assets
                ./firmware/src/splash_animations.h
                ./tools/packaging/icons
                ./tools/packaging/linux
              ];
            };
            cargoRoot = "desktop";
            buildAndTestSubdir = "desktop";
            # Includes the vendored xattr ([patch.crates-io] path dependency).
            cargoLock.lockFile = ./desktop/Cargo.lock;
            cargoBuildFlags = [
              "-p"
              "clawdmeter-app"
            ];
            # The workspace tests run in CI; GPUI's need a display.
            doCheck = false;

            nativeBuildInputs = [
              pkgs.pkg-config
              pkgs.rustPlatform.bindgenHook
            ];
            buildInputs = linkLibs pkgs;

            postInstall = ''
              install -Dm644 tools/packaging/linux/clawdmeter.desktop -t $out/share/applications
              for s in 16 24 32 48 64 128 256 512; do
                install -Dm644 tools/packaging/icons/clawdmeter-$s.png \
                  $out/share/icons/hicolor/''${s}x$s/apps/clawdmeter.png
              done
            '';

            # rpath, not a wrapper: the app writes its own path into Claude
            # Code's hooks and the autostart entry, and a wrapper's
            # `.clawdmeter-wrapped` would run there without the environment.
            # Runs after the fixup's rpath shrinking, which would drop
            # libraries that are only dlopen()ed.
            postFixup = ''
              patchelf --add-rpath ${lib.makeLibraryPath (runtimeLibs pkgs ++ linkLibs pkgs)} \
                $out/bin/clawdmeter
            '';

            meta = {
              description = "Desktop companion for the Clawdmeter Claude Code usage monitor";
              homepage = "https://github.com/HermannBjorgvin/Clawdmeter";
              license = lib.licenses.mit;
              mainProgram = "clawdmeter";
              platforms = systems;
            };
          };
          default = clawdmeter;
        }
      );

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.clawdmeter}/bin/clawdmeter";
          meta.description = "Clawdmeter desktop app";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.clawdmeter ];
          packages = with pkgs; [
            clippy
            rustfmt
            rust-analyzer
            platformio # firmware: pio run -d firmware -e <env>
            python3 # tools/ci, tools/packaging
            SDL2 # firmware simulator (-e sim)
          ];
          # `cargo run` finds Vulkan and Wayland like the packaged binary does.
          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath (runtimeLibs pkgs ++ linkLibs pkgs);
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
