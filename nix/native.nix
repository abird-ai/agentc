{
  pkgs,
  lib,
  self,
  systems,
  version,
}:
pkgs.stdenv.mkDerivation {
  pname = "agentc";
  inherit version;
  src = self;

  # clang + make are the whole toolchain; python3 generates the static plugin
  # registry (optional). This is the plain host build.
  nativeBuildInputs = [
    pkgs.clang
    pkgs.gnumake
    pkgs.python3
  ];

  # freestanding binary: no libc, so stdenv hardening (fortify turns memset
  # into __memset_chk) must be off.
  hardeningDisable = ["all"];

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
}
