{
  pkgs,
  lib,
  native,
  sourceRoot,
  version,
}: let
  inherit (pkgs) llvmPackages;

  # Directories that are never linted: vendored code, build output, the
  # historical agent run records.
  prune = [".git" "third_party" "build" "dist" "runs"];

  # A source tree holding only `exts` plus the tool config the linter reads.
  lintTree = {
    exts,
    keep ? [],
  }:
    lib.cleanSourceWith {
      src = sourceRoot;
      filter = path: type:
        if type == "directory"
        then !(lib.elem (baseNameOf path) prune)
        else
          lib.any (ext: lib.hasSuffix ext path) exts
          || lib.elem (baseNameOf path) keep;
    };

  # `alejandra --check` over flake.nix and nix/ (the same formatter the dev
  # shell and `nix fmt` use).
  alejandra =
    pkgs.runCommand "agentc-alejandra" {
      nativeBuildInputs = [pkgs.alejandra];
    } ''
      alejandra --check ${lintTree {exts = [".nix"];}}
      touch "$out"
    '';

  # markdownlint over every owned .md, driven by .markdownlint-cli2.jsonc.
  markdownlint =
    pkgs.runCommand "agentc-markdownlint" {
      nativeBuildInputs = [pkgs.markdownlint-cli2];
    } ''
      cd ${lintTree {
        exts = [".md"];
        keep = [".markdownlint-cli2.jsonc"];
      }}
      markdownlint-cli2
      touch "$out"
    '';

  # clang-format conformance for the C23 sources (the style lives in
  # .clang-format).
  clangFormat =
    pkgs.runCommand "agentc-clang-format" {
      nativeBuildInputs = [llvmPackages.clang-tools];
    } ''
      cd ${lintTree {
        exts = [".c" ".h"];
        keep = [".clang-format"];
      }}
      clang-format --style=file --dry-run --Werror $(find . -name '*.c' -o -name '*.h')
      touch "$out"
    '';

  # clang-tidy over every shipped translation unit. bear records the exact
  # Makefile flags, clang-tidy reads .clang-tidy. The vendored mbedTLS units and
  # the tests are dropped from the list (mbedTLS is third-party and is not ours
  # to rewrite; the tests are validated by running them, and the analyzer's
  # global/function-pointer tracking produces false positives there).
  clangTidy = pkgs.stdenv.mkDerivation {
    pname = "agentc-clang-tidy";
    inherit version;
    src = sourceRoot;

    nativeBuildInputs = [
      pkgs.bear
      pkgs.clang
      pkgs.gnumake
      pkgs.python3
      llvmPackages.clang-tools
    ];

    # freestanding binary: fortify's __memset_chk has no implementation here.
    hardeningDisable = ["all"];
    dontConfigure = true;

    buildPhase = ''
      runHook preBuild
      rm -rf build
      bear -- make -j$NIX_BUILD_CORES CC=clang release
      runHook postBuild
    '';

    doCheck = true;
    checkPhase = ''
      runHook preCheck
      python3 -c "import json; [print(e['file']) for e in json.load(open('compile_commands.json')) if not e['file'].startswith(('third_party/', 'tests/'))]" > tidylist
      echo "clang-tidy: $(wc -l < tidylist) translation units"
      xargs -P "$NIX_BUILD_CORES" -n1 clang-tidy -p . --quiet < tidylist
      runHook postCheck
    '';

    installPhase = ''touch "$out"'';
  };
in {
  # The strict native build is the package gate.
  package = native;
  inherit alejandra markdownlint clangFormat clangTidy;
}
