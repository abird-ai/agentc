{
  description = "agentc — a minimal, extensible coding agent in freestanding C23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems f;

      # Cross targets.
      #
      # Everything here is a *freestanding* build: no libc, no CRT, own _start,
      # so no cross libc or sysroot is needed. clang plus a cross-capable linker
      # is the whole toolchain, which is why these are plain per-arch flags
      # rather than pkgsCross package sets.
      #
      # macOS is different: the port links libSystem, so it needs the Apple SDK
      # (fetched below) and LLVM's Mach-O linker instead of a libc-free link.
      linuxTargets = {
        agentc-linux-x86_64 = {
          triple = "x86_64-unknown-linux-gnu";
          archFlags = "-fno-pic -mno-red-zone";
          out = "agentc";
        };
        agentc-linux-aarch64 = {
          triple = "aarch64-unknown-linux-gnu";
          archFlags = "-fno-pic";
          out = "agentc";
        };
        agentc-linux-riscv64 = {
          triple = "riscv64-unknown-linux-gnu";
          archFlags = "-fno-pic";
          out = "agentc";
        };
      };
      windowsTargets = {
        agentc-windows-x86_64 = {
          out = "agentc.exe";
          winArch = "x86_64";
        };
        agentc-windows-aarch64 = {
          out = "agentc.exe";
          winArch = "arm64";
        };
      };
      # Both slices plus a universal binary; the SDK is fetched from Apple's CDN.
      macosTargets = {
        agentc-macos-arm64 = {
          arch = "arm64";
        };
        agentc-macos-x86_64 = {
          arch = "x86_64";
        };
      };
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          lib = pkgs.lib;

          # The static binary for one target; `.exe` names on Windows.
          mkBinary =
            name: args:
            pkgs.stdenv.mkDerivation {
              pname = name;
              version = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./VERSION);
              src = self;

              # clang + lld are the toolchain; make drives the build; python3
              # generates the static plugin registry (optional).
              nativeBuildInputs = [
                pkgs.clang
                pkgs.lld
                pkgs.llvm
                pkgs.gnumake
                pkgs.python3
              ];

              # freestanding binary: no libc, so stdenv hardening (fortify turns
              # memset into __memset_chk) must be off.
              hardeningDisable = [ "all" ];
              dontConfigure = true;

              buildPhase =
                if args.windows or false then
                  ''
                    runHook preBuild
                    make windows \
                      WIN_CC="${pkgs.clang}/bin/clang" \
                      WIN_LD="${pkgs.lld}/bin/lld-link" \
                      WIN_DLLTOOL="${pkgs.llvm}/bin/llvm-dlltool" \
                      WIN_ARCH="${args.winArch or "x86_64"}" \
                      WIN_OUT=build/${args.out}
                    runHook postBuild
                  ''
                else
                  ''
                    runHook preBuild
                    make release \
                      CC="${pkgs.clang}/bin/clang --target=${args.triple}" \
                      ARCH_FLAGS="${args.archFlags}" \
                      REL_OBJDIR=build/obj/${name}/release \
                      OUT=build/${args.out} \
                      REL_LDFLAGS_EXTRA="-fuse-ld=${pkgs.lld}/bin/ld.lld"
                    runHook postBuild
                  '';

              # one directory per target, so `nix build .#release-all` can carry
              # every architecture without the names colliding
              installPhase = ''
                runHook preInstall
                install -Dm0755 build/${args.out} \
                  $out/bin/${name}${lib.optionalString (args.windows or false) ".exe"}
                runHook postInstall
              '';

              meta = {
                description =
                  "agentc for ${args.triple or "windows-${args.winArch or "x86_64"}"}";
                homepage = "https://github.com/pvl/agentc";
                license = lib.licenses.mit;
                platforms = [ system ];
              };
            };

          # Apple's SDK ships as a .pkg; nixpkgs knows how to unpack it. Pinned
          # by hash so the release binaries stay reproducible.
          macosSdk =
            pkgs.callPackage "${nixpkgs}/pkgs/by-name/ap/apple-sdk/common/fetch-sdk.nix" { }
              {
                urls = [
                  "https://swcdn.apple.com/content/downloads/14/48/052-59890-A_I0F5YGAY0Y/p9n40hio7892gou31o1v031ng6fnm9sb3c/CLTools_macOSNMOS_SDK.pkg"
                  "https://web.archive.org/web/20250211001355/https://swcdn.apple.com/content/downloads/14/48/052-59890-A_I0F5YGAY0Y/p9n40hio7892gou31o1v031ng6fnm9sb3c/CLTools_macOSNMOS_SDK.pkg"
                ];
                version = "14.4";
                hash = "sha256-QozDiwY0Czc0g45vPD7G4v4Ra+3DujCJbSads3fJjjM=";
              };

          # The macOS port keeps the SDK's headers and its libSystem link, so the
          # cross toolchain is clang + lld driving `make PLATFORM=mac`.
          mkMac =
            name: args:
            let
              # clang only passes the Mach-O specific flags (-arch,
              # -platform_version) when it resolves the linker itself, so lld
              # goes on PATH and is selected by name, not by absolute path.
              cc = pkgs.writeShellScript "agentc-${args.arch}-apple-darwin-cc" ''
                export PATH="${pkgs.llvmPackages.lld}/bin:$PATH"
                exec ${pkgs.llvmPackages.clang-unwrapped}/bin/clang \
                  --target=${args.arch}-apple-darwin \
                  -isysroot ${macosSdk} -mmacosx-version-min=12.0 \
                  -fuse-ld=lld "$@"
              '';
            in
            pkgs.stdenv.mkDerivation {
              pname = name;
              version = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./VERSION);
              src = self;
              nativeBuildInputs = [
                pkgs.llvmPackages.clang-unwrapped
                pkgs.llvmPackages.lld
                pkgs.llvmPackages.llvm
                pkgs.gnumake
                pkgs.python3
              ];
              hardeningDisable = [ "all" ];
              dontConfigure = true;
              buildPhase = ''
                runHook preBuild
                make release PLATFORM=mac \
                  CC="${cc}" \
                  MAC_ARCH=${args.arch} \
                  OUT=build/${name} \
                  REL_OBJDIR=build/obj/${name}/release
                runHook postBuild
              '';
              installPhase = ''
                runHook preInstall
                install -Dm0755 build/${name} $out/bin/${name}
                runHook postInstall
              '';
              meta = {
                description = "agentc for macOS ${args.arch}";
                homepage = "https://github.com/pvl/agentc";
                license = lib.licenses.mit;
                platforms = [ system ];
              };
            };

          native = pkgs.stdenv.mkDerivation {
            pname = "agentc";
            version = builtins.replaceStrings [ "\n" ] [ "" ] (builtins.readFile ./VERSION);
            src = self;
            nativeBuildInputs = [
              pkgs.clang
              pkgs.gnumake
              pkgs.python3
            ];
            hardeningDisable = [ "all" ];
            buildPhase = "make release CC=clang";
            installPhase = ''
              runHook preInstall
              install -Dm0755 build/agentc $out/bin/agentc
              runHook postInstall
            '';
            meta = {
              description = "Minimal, extensible coding agent in freestanding C23";
              homepage = "https://github.com/pvl/agentc";
              license = lib.licenses.mit;
              mainProgram = "agentc";
              platforms = systems;
            };
          };

          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          isDarwin = pkgs.stdenv.hostPlatform.isDarwin;

          # windowsTargets entries are tagged so mkBinary takes the lld-link path.
          cross = lib.mapAttrs (
            name: args:
            mkBinary name (args // { windows = builtins.hasAttr name windowsTargets; })
          ) (lib.optionalAttrs isLinux (linuxTargets // windowsTargets));

          # The macOS port builds from any host once the SDK is there; on a Mac
          # itself the same target builds natively (no cross flags involved).
          macos = lib.mapAttrs (name: args: mkMac name args) (
            lib.optionalAttrs (isLinux || isDarwin) macosTargets
          );

          # One download for both Mac slices.
          macosUniversal = pkgs.runCommand "agentc-macos-universal" {
            nativeBuildInputs = [ pkgs.llvmPackages.llvm ];
          } ''
            mkdir -p $out/bin
            llvm-lipo -create ${macos.agentc-macos-arm64}/bin/agentc-macos-arm64 \
              ${macos.agentc-macos-x86_64}/bin/agentc-macos-x86_64 \
              -output $out/bin/agentc-macos-universal
          '';

          all = pkgs.symlinkJoin {
            name = "agentc-all";
            paths = [ native ] ++ lib.attrValues cross ++ lib.attrValues macos
              ++ lib.optional (macos != { }) macosUniversal;
          };
        in
        {
          default = native;
          inherit native all macosUniversal;
          inherit (macos) agentc-macos-arm64 agentc-macos-x86_64;
          # the release workflow builds .#release-all
          release-all = all;
        }
        // cross
      );

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.native}/bin/agentc";
        };
      });

      devShells = forAllSystems (system: {
        default = nixpkgs.legacyPackages.${system}.mkShell {
          packages =
            (with nixpkgs.legacyPackages.${system}; [
              clang
              lld
              llvm
              gnumake
              python3
              cargo
              rustc
              qemu   # runs the aarch64/riscv64 build in the test suite
            ])
            # Wine runs the Windows golden suite on Linux (tests/wine.sh). It is
            # Linux-only; the macOS shells cross-build but do not run PE files.
            ++ (nixpkgs.lib.optional
              (system == "x86_64-linux" || system == "aarch64-linux")
              nixpkgs.legacyPackages.${system}.wine64
            );
          # clang's setup hook exports NIX_HARDENING_ENABLE, whose
          # -fzero-call-used-regs is an x86-only flag that hard-errors when the
          # same shell cross-compiles for riscv64. The flake's build derivations
          # already pass hardeningDisable = [ "all" ]; do the same here.
          shellHook = ''
            # stdenv exports CC=gcc while this tree requires clang (including
            # for the Windows cross build); let the Makefile pick clang unless
            # the caller overrides CC explicitly.
            unset CC CXX
            unset NIX_HARDENING_ENABLE
            echo "agentc dev shell: make / make check / make windows / make wine-check"
          '';
        };
      });

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt-rfc-style);
    };
}
