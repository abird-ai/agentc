{
  description = "agentc — a minimal, extensible coding agent in freestanding C23";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs = inputs:
    import ./nix {
      inherit inputs;
      sourceRoot = ./.;
    };
}
