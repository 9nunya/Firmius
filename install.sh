#!/bin/sh
# Firmius installer
# Usage: curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh

set -eu

REPO="${FIRMIUS_REPO:-9nunya/Firmius}"
VERSION="${FIRMIUS_VERSION:-latest}"
INSTALL_DIR="${FIRMIUS_INSTALL_DIR:-}"
FROM_SOURCE=0

say() { printf '%s\n' "$*"; }
fail() { say "\n✖ $*" >&2; exit 1; }
info() { say "  $*"; }

usage() {
  cat <<'EOF'
Firmius installer

Install the latest prebuilt Firmius binary:
  curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh

Options:
  --dir DIR       Install into DIR
  --version TAG   Install a release tag, for example v0.1.0
  --source        Build from source with cargo instead of downloading a binary
  --help          Show this help

Environment variables: FIRMIUS_INSTALL_DIR, FIRMIUS_VERSION, FIRMIUS_REPO
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --dir) [ "$#" -ge 2 ] || fail "--dir needs a directory"; INSTALL_DIR=$2; shift 2 ;;
    --version) [ "$#" -ge 2 ] || fail "--version needs a tag"; VERSION=$2; shift 2 ;;
    --source) FROM_SOURCE=1; shift ;;
    --help|-h) usage; exit 0 ;;
    *) fail "unknown option: $1 (try --help)" ;;
  esac
done

command -v curl >/dev/null 2>&1 || fail "curl is required. Install curl and try again."

if [ -z "$INSTALL_DIR" ]; then
  case "$(uname -s 2>/dev/null || echo unknown)" in
    Darwin) INSTALL_DIR="${HOME}/.local/bin" ;;
    *) INSTALL_DIR="${HOME}/.local/bin" ;;
  esac
fi

if [ "$FROM_SOURCE" -eq 1 ]; then
  command -v cargo >/dev/null 2>&1 || fail "--source requires Rust and Cargo. Install from https://rustup.rs/ first."
  say "\n  Building Firmius from source..."
  cargo install --locked --git "https://github.com/$REPO.git" --bin firmius firmius
  say "\n  ✓ Firmius installed with Cargo."
  say "  Make sure Cargo's bin directory is on PATH, then run: firmius"
  exit 0
fi

OS=$(uname -s 2>/dev/null || echo unknown)
ARCH=$(uname -m 2>/dev/null || echo unknown)
case "$OS:$ARCH" in
  Darwin:x86_64) TARGET=x86_64-apple-darwin ;;
  Darwin:arm64|Darwin:aarch64) TARGET=aarch64-apple-darwin ;;
  Linux:x86_64|Linux:amd64) TARGET=x86_64-unknown-linux-gnu ;;
  Linux:aarch64|Linux:arm64) TARGET=aarch64-unknown-linux-gnu ;;
  MINGW*:x86_64|MSYS*:x86_64|CYGWIN*:x86_64) TARGET=x86_64-pc-windows-msvc ;;
  *)
    fail "No prebuilt binary for $OS/$ARCH. Retry with --source after installing Rust, or see https://github.com/$REPO/releases."
    ;;
esac

case "$VERSION" in
  latest) BASE="https://github.com/$REPO/releases/latest/download" ;;
  v*) BASE="https://github.com/$REPO/releases/download/$VERSION" ;;
  *) BASE="https://github.com/$REPO/releases/download/v$VERSION" ;;
esac

EXT=tar.gz
case "$TARGET" in *windows*) EXT=zip ;; esac
ASSET="firmius-$TARGET.$EXT"
TMP=$(mktemp -d 2>/dev/null || mktemp -d -t firmius)
trap 'rm -rf "$TMP"' EXIT
ARCHIVE="$TMP/$ASSET"
CHECKSUMS="$TMP/SHA256SUMS"

say "\n  ┌──────────────────────────────────────────┐"
say "  │              FIRMIUS INSTALLER           │"
say "  └──────────────────────────────────────────┘"
info "Platform: $TARGET"
info "Destination: $INSTALL_DIR/firmius"
info "Downloading $ASSET..."

if ! curl --fail --location --silent --show-error --retry 3 --output "$ARCHIVE" "$BASE/$ASSET"; then
  fail "Could not download a release for $TARGET. Try --source or visit https://github.com/$REPO/releases."
fi

if curl --fail --location --silent --show-error --retry 3 --output "$CHECKSUMS" "$BASE/SHA256SUMS" 2>/dev/null; then
  EXPECTED=$(awk -v file="$ASSET" '$2 == file || $2 == "*" file { print $1; exit }' "$CHECKSUMS")
  if [ -n "$EXPECTED" ]; then
    ACTUAL=$(shasum -a 256 "$ARCHIVE" 2>/dev/null | awk '{print $1}' || true)
    if [ -z "$ACTUAL" ]; then ACTUAL=$(sha256sum "$ARCHIVE" 2>/dev/null | awk '{print $1}' || true); fi
    [ -z "$ACTUAL" ] || [ "$EXPECTED" = "$ACTUAL" ] || fail "Checksum verification failed."
  fi
fi

mkdir -p "$TMP/unpacked" "$INSTALL_DIR"
case "$EXT" in
  tar.gz) tar -xzf "$ARCHIVE" -C "$TMP/unpacked" ;;
  zip)
    command -v unzip >/dev/null 2>&1 || fail "unzip is required to install the Windows archive."
    unzip -q "$ARCHIVE" -d "$TMP/unpacked" ;;
esac

BINARY=$(find "$TMP/unpacked" -type f \( -name firmius -o -name firmius.exe \) -print | head -n 1)
[ -n "$BINARY" ] || fail "The release archive did not contain a firmius binary."
cp "$BINARY" "$INSTALL_DIR/firmius$(case "$TARGET" in *windows*) printf '.exe';; esac)"
chmod +x "$INSTALL_DIR/firmius" 2>/dev/null || true

say "\n  ✓ Firmius installed successfully."
case ":${PATH:-}:" in
  *":$INSTALL_DIR:"*) ;;
  *)
    info "Add this directory to your PATH:"
    say "    export PATH=\"$INSTALL_DIR:\$PATH\""
    ;;
esac
say "  Run: firmius"
