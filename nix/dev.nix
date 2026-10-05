{
  pkgs,
  lib,
  system,
}: {
  shell = pkgs.mkShell {
    packages =
      (with pkgs; [
        clang
        lld
        llvm
        gnumake
        python3
        cargo
        rustc
        qemu # runs the aarch64/riscv64 build in the test suite
        # linting: clang-format/clang-tidy for C23, cppcheck, markdownlint and
        # alejandra for the docs/flake (see nix/checks.nix)
        clang-tools
        cppcheck
        markdownlint-cli2
        alejandra
      ])
      # Wine runs the Windows golden suite on Linux (tests/wine.sh). It is
      # Linux-only; the macOS shells cross-build but do not run PE files.
      ++ (lib.optional (system == "x86_64-linux" || system == "aarch64-linux") pkgs.wine64);

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

  formatter = pkgs.alejandra;
}
