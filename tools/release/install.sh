#!/usr/bin/env bash
# Installs the shvi CLI (a single binary) into ~/.shvi/bin and the matching ScriptHook VI SDK.
#   curl -fsSL https://github.com/modpapi/scripthook-vi/releases/latest/download/install.sh | bash
#   ... | bash -s -- --version 0.2.0          a specific release
# Env: SHVI_REPO (owner/name), SHVI_HOME (default ~/.shvi).
set -euo pipefail

REPO="${SHVI_REPO:-modpapi/scripthook-vi}"
HOME_DIR="${SHVI_HOME:-$HOME/.shvi}"
VERSION=""
while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2#v}"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

case "$(uname -s)" in
  Linux) os=linux ;;
  Darwin) os=darwin ;;
  *) echo "unsupported OS $(uname -s); on Windows use install.ps1" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) arch=x64 ;;
  arm64|aarch64) arch=arm64 ;;
  *) echo "unsupported CPU $(uname -m)" >&2; exit 1 ;;
esac
[ "$os" = linux ] && [ "$arch" = arm64 ] && { echo "no linux-arm64 build yet" >&2; exit 1; }

if [ -n "$VERSION" ]; then base="https://github.com/$REPO/releases/download/v$VERSION"
else base="https://github.com/$REPO/releases/latest/download"; fi

mkdir -p "$HOME_DIR/bin"
echo "› downloading shvi-$os-$arch from $base"
curl -fsSL "$base/shvi-$os-$arch" -o "$HOME_DIR/bin/shvi.tmp"
chmod +x "$HOME_DIR/bin/shvi.tmp"
mv "$HOME_DIR/bin/shvi.tmp" "$HOME_DIR/bin/shvi"
v="$("$HOME_DIR/bin/shvi" --version)"
echo "✓ shvi $v installed: $HOME_DIR/bin/shvi"
"$HOME_DIR/bin/shvi" sdk install "$v"

case ":$PATH:" in
  *":$HOME_DIR/bin:"*) ;;
  *) echo
     echo "Add shvi to your PATH (then open a new shell):"
     echo "  echo 'export PATH=\"$HOME_DIR/bin:\$PATH\"' >> ~/.bashrc" ;;
esac
echo "Next: shvi doctor   then   shvi new"
