{
  inputs,
  sourceRoot,
}: let
  inherit (inputs) nixpkgs self;
  lib = nixpkgs.lib;
  version = lib.removeSuffix "\n" (builtins.readFile (sourceRoot + "/VERSION"));
  systems = [
    "x86_64-linux"
    "aarch64-linux"
    "aarch64-darwin"
  ];
  forAllSystems = f: lib.genAttrs systems f;

  perSystem = system: let
    pkgs = nixpkgs.legacyPackages.${system};
    native = import ./native.nix {inherit pkgs lib self systems version;};
    cross = import ./cross.nix {inherit pkgs lib self nixpkgs version;};
    release = import ./release.nix {
      inherit pkgs lib native;
      inherit (cross) cross macos macosUniversal;
    };
    dev = import ./dev.nix {inherit pkgs lib system;};
    checks = import ./checks.nix {inherit pkgs lib native sourceRoot version;};
    packages =
      {
        default = native;
        inherit native;
        inherit (release) all;
        inherit (cross) macosUniversal;
        inherit (cross.macos) agentc-macos-arm64 agentc-macos-x86_64;
        release-all = release.all;
      }
      // cross.cross;
  in {
    inherit packages checks;
    apps.default = {
      type = "app";
      program = "${native}/bin/agentc";
    };
    inherit (dev) formatter;
    devShells.default = dev.shell;
  };
in {
  packages = forAllSystems (system: (perSystem system).packages);
  apps = forAllSystems (system: (perSystem system).apps);
  devShells = forAllSystems (system: (perSystem system).devShells);
  formatter = forAllSystems (system: (perSystem system).formatter);
  checks = forAllSystems (system: (perSystem system).checks);
}
