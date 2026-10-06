#!/bin/sh
# agentc installer — POSIX (Linux x86_64, macOS arm64)
#
#   curl -fsSL https://raw.githubusercontent.com/<repo>/main/install.sh | sh
#   AGENTC_REPO=owner/agentc AGENTC_VERSION=v0.1.0 sh install.sh
#
# Environment:
#   AGENTC_REPO     GitHub repo (default: pvl/agentc)
#   AGENTC_VERSION  tag like v0.1.0, or "latest" (default)
#   AGENTC_PREFIX   install prefix (default: ~/.local)
set -e

REPO="${AGENTC_REPO:-pvl/agentc}"
VERSION="${AGENTC_VERSION:-latest}"
PREFIX="${AGENTC_PREFIX:-$HOME/.local}"

os=$(uname -s)
arch=$(uname -m)
case "$os" in
Linux)
    [ "$arch" = "x86_64" ] || { echo "agentc: only x86_64 Linux is published (got $arch)" >&2; exit 1; }
    asset="agentc-linux-x86_64.tar.gz"
    ;;
Darwin)
    [ "$arch" = "arm64" ] || { echo "agentc: only arm64 macOS is published (got $arch)" >&2; exit 1; }
    asset="agentc-macos-arm64.tar.gz"
    ;;
*)
    echo "agentc: unsupported OS '$os' (on Windows use install.ps1)" >&2
    exit 1
    ;;
esac

if [ "$VERSION" = "latest" ]; then
    url="https://github.com/$REPO/releases/latest/download/$asset"
else
    url="https://github.com/$REPO/releases/download/$VERSION/$asset"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "agentc: downloading $url"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL --proto '=https' --proto-redir '=https' "$url" -o "$tmp/$asset"
elif command -v wget >/dev/null 2>&1; then
    wget -qO "$tmp/$asset" "$url"
else
    echo "agentc: curl or wget is required" >&2
    exit 1
fi

tar -xzf "$tmp/$asset" -C "$tmp"
[ -f "$tmp/agentc" ] || { echo "agentc: archive did not contain agentc" >&2; exit 1; }

mkdir -p "$PREFIX/bin"
install -m 0755 "$tmp/agentc" "$PREFIX/bin/agentc"
echo "agentc: installed $PREFIX/bin/agentc"
"$PREFIX/bin/agentc" --version

case ":$PATH:" in
*":$PREFIX/bin:"*) ;;
*) echo "agentc: add $PREFIX/bin to your PATH to run 'agentc'" ;;
esac
