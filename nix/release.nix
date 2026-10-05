{
  pkgs,
  lib,
  native,
  cross,
  macos,
  macosUniversal,
}: {
  # One installable covering the host build and every cross target; this is
  # what `nix build .#release-all` (the release workflow) consumes.
  all = pkgs.symlinkJoin {
    name = "agentc-all";
    paths =
      [native]
      ++ lib.attrValues cross
      ++ lib.attrValues macos
      ++ lib.optional (macos != {}) macosUniversal;
  };
}
