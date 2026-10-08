#!/bin/sh
# agentc installer: fetch a GitHub release archive, verify its SHA-256 and
# install the binary. No build toolchain required.
#
#   curl -fsSL https://raw.githubusercontent.com/abird-ai/agentc/master/install.sh | sh
#
# Environment overrides:
#   AGENTC_VERSION           release to install (default "latest"; "0.6.0" or "v0.6.0")
#   AGENTC_INSTALL_DIR      target directory (default "$HOME/.local/bin")
#   AGENTC_REPO              GitHub owner/repo (default "abird-ai/agentc")
#   AGENTC_RELEASE_BASE_URL  override the download base URL
set -eu

say() { printf '%s\n' "$*"; }
die() { printf 'agentc installer: %s\n' "$*" >&2; exit 1; }

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  else
    die "sha256sum or shasum is required for checksum verification"
  fi
}

agentc_version() {
  out="$("$1" --version 2>/dev/null || true)"
  case "$out" in
    "agentc "*) printf '%s' "${out#agentc }" ;;
  esac
}

VERSION="${AGENTC_VERSION:-latest}"
REPO="${AGENTC_REPO:-abird-ai/agentc}"
BASE_URL="${AGENTC_RELEASE_BASE_URL:-}"

if [ -n "${AGENTC_INSTALL_DIR:-}" ]; then
  INSTALL_DIR="$AGENTC_INSTALL_DIR"
elif [ -n "${HOME:-}" ]; then
  INSTALL_DIR="$HOME/.local/bin"
else
  die "HOME is not set; set AGENTC_INSTALL_DIR explicitly"
fi

EXT=".tar.gz"
BIN="agentc"
case "$(uname -s 2>/dev/null || printf unknown)" in
  Linux)
    case "$(uname -m 2>/dev/null || printf unknown)" in
      x86_64|amd64)   ASSET="agentc-linux-x86_64" ;;
      aarch64|arm64)  ASSET="agentc-linux-aarch64" ;;
      riscv64)        ASSET="agentc-linux-riscv64" ;;
      *) die "unsupported Linux architecture: $(uname -m). Published: x86_64, aarch64, riscv64." ;;
    esac
    ;;
  Darwin)
    case "$(uname -m 2>/dev/null || printf unknown)" in
      arm64|aarch64)  ASSET="agentc-macos-arm64" ;;
      x86_64|amd64)   ASSET="agentc-macos-x86_64" ;;
      *) die "unsupported macOS architecture: $(uname -m). Published: arm64, x86_64." ;;
    esac
    ;;
  MINGW*|MSYS*|CYGWIN*)
    EXT=".zip"
    BIN="agentc.exe"
    case "$(uname -m 2>/dev/null || printf unknown)" in
      x86_64|amd64)   ASSET="agentc-windows-x86_64" ;;
      aarch64|arm64)  ASSET="agentc-windows-aarch64" ;;
      *) die "unsupported Windows architecture: $(uname -m). Published: x86_64, arm64." ;;
    esac
    ;;
  *)
    die "unsupported OS: $(uname -s 2>/dev/null || printf unknown)"
    ;;
esac
ARCHIVE="$ASSET$EXT"

if [ -z "$BASE_URL" ]; then
  if [ "$VERSION" = "latest" ]; then
    BASE_URL="https://github.com/${REPO}/releases/latest/download"
  else
    case "$VERSION" in
      v*) TAG="$VERSION" ;;
      *)  TAG="v$VERSION" ;;
    esac
    BASE_URL="https://github.com/${REPO}/releases/download/${TAG}"
  fi
fi

command -v curl >/dev/null 2>&1 || die "curl is required"

TMPROOT="${TMPDIR:-/tmp}"
WORKDIR="$(mktemp -d "${TMPROOT%/}/agentc-install.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT HUP INT TERM

say "Downloading agentc..."
curl -fsSL "${BASE_URL%/}/${ARCHIVE}" -o "$WORKDIR/$ARCHIVE"
curl -fsSL "${BASE_URL%/}/${ARCHIVE}.sha256" -o "$WORKDIR/$ARCHIVE.sha256"

expected="$(awk 'NR == 1 { print $1; exit }' "$WORKDIR/$ARCHIVE.sha256" | tr 'A-F' 'a-f')"
[ "${#expected}" -eq 64 ] || die "invalid SHA-256 sidecar"
case "$expected" in
  *[!0-9a-f]*) die "invalid SHA-256 sidecar" ;;
esac
actual="$(sha256_file "$WORKDIR/$ARCHIVE" | tr 'A-F' 'a-f')"
[ "$expected" = "$actual" ] || die "SHA-256 verification failed"

mkdir -p "$WORKDIR/extract"
case "$EXT" in
  .zip)
    if command -v unzip >/dev/null 2>&1; then
      unzip -q -o "$WORKDIR/$ARCHIVE" -d "$WORKDIR/extract"
    else
      tar -xf "$WORKDIR/$ARCHIVE" -C "$WORKDIR/extract"
    fi
    ;;
  *)
    tar -xzf "$WORKDIR/$ARCHIVE" -C "$WORKDIR/extract"
    ;;
esac
SRC="$WORKDIR/extract/$BIN"
[ -f "$SRC" ] || die "archive does not contain $BIN"
chmod +x "$SRC"
bin_hash="$(sha256_file "$SRC" | tr 'A-F' 'a-f')"

downloaded="$(agentc_version "$SRC")"
[ -n "$downloaded" ] || die "downloaded binary did not report an agentc version"
if [ "$VERSION" != "latest" ]; then
  requested="${VERSION#v}"
  [ "$downloaded" = "$requested" ] || die "downloaded version $downloaded does not match requested $requested"
fi

DEST="$INSTALL_DIR/$BIN"
mkdir -p "$INSTALL_DIR"
if [ -f "$DEST" ] && [ "$(sha256_file "$DEST" | tr 'A-F' 'a-f')" = "$bin_hash" ]; then
  say "agentc $downloaded is already up to date at $DEST"
  exit 0
fi
tmp="$INSTALL_DIR/.$BIN.tmp.$$"
cp "$SRC" "$tmp"
chmod 0755 "$tmp"
mv -f "$tmp" "$DEST"

say "Installed agentc $downloaded to $DEST"
case ":${PATH:-}:" in
  *":$INSTALL_DIR:"*) ;;
  *) say "Add $INSTALL_DIR to PATH to run 'agentc'." ;;
esac
say "Next: agentc setup    # pick a provider, store credentials, pick a model"
