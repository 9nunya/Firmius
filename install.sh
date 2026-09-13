#!/bin/sh
# Firmius installer
# Usage: curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh

set -eu

REPO="${FIRMIUS_REPO:-9nunya/Firmius}"
VERSION="${FIRMIUS_VERSION:-latest}"
INSTALL_DIR="${FIRMIUS_INSTALL_DIR:-}"
FROM_SOURCE=0

say() { printf '%s\n' "$*"; }
fail() { say "" >&2; say "✖ $*" >&2; exit 1; }
info() { say "  $*"; }

stop_running_daemon() {
  candidate=$1
  daemon_root=${FIRMIUS_DATA_DIR:-$HOME/.firmius}
  if [ -x "$candidate" ] && [ -f "$daemon_root/daemon.lock" ]; then
    info "Stopping the running Firmius daemon before replacing the shared executable..."
    "$candidate" daemon-stop || fail "Could not stop the running Firmius daemon; refusing to replace its executable."
  fi
}

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

case "$REPO" in
  ''|*/*/*|/*|*/) fail "FIRMIUS_REPO must contain exactly one non-empty owner/repository pair." ;;
  */*) ;;
  *) fail "FIRMIUS_REPO must be an owner/repository name." ;;
esac
REPO_OWNER=${REPO%%/*}
REPO_NAME=${REPO#*/}
case "$REPO_OWNER" in *[!A-Za-z0-9_.-]*) fail "FIRMIUS_REPO contains unsafe characters." ;; esac
case "$REPO_NAME" in *[!A-Za-z0-9_.-]*) fail "FIRMIUS_REPO contains unsafe characters." ;; esac
case "$VERSION" in
  latest) ;;
  v*) RELEASE_VERSION=${VERSION#v} ;;
  *) RELEASE_VERSION=$VERSION ;;
esac
case "$VERSION" in *[!A-Za-z0-9._-]*) fail "FIRMIUS_VERSION contains unsafe characters." ;; esac
if [ "$VERSION" != latest ] && ! printf '%s\n' "$RELEASE_VERSION" | awk '
  /^[0-9][0-9]*([.][0-9][0-9]*)*$/ { valid = 1 }
  END { exit(valid ? 0 : 1) }
'; then
  fail "FIRMIUS_VERSION must be latest or a numeric release tag (for example v1.2.3)."
fi

command -v curl >/dev/null 2>&1 || fail "curl is required. Install curl and try again."

if [ -z "$INSTALL_DIR" ]; then
  case "$(uname -s 2>/dev/null || echo unknown)" in
    Darwin) INSTALL_DIR="${HOME}/.local/bin" ;;
    *) INSTALL_DIR="${HOME}/.local/bin" ;;
  esac
fi

if [ "$FROM_SOURCE" -eq 1 ]; then
  command -v cargo >/dev/null 2>&1 || fail "--source requires Rust and Cargo. Install from https://rustup.rs/ first."
  CARGO_BIN="${CARGO_HOME:-$HOME/.cargo}/bin"
  stop_running_daemon "$CARGO_BIN/firmius"
  say ""
  say "  Building Firmius from source..."
  cargo install --locked --git "https://github.com/$REPO.git" --bin firmius firmius
  MARKER_TMP="$CARGO_BIN/.firmius-install.json.$$"
  printf '{"channel":"cargo-git","repo":"%s","version":"source"}\n' "$REPO" > "$MARKER_TMP"
  mv -f "$MARKER_TMP" "$CARGO_BIN/firmius-install.json"
  say ""
  say "  ✓ Firmius installed with Cargo."
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

say ""
say "  ┌──────────────────────────────────────────┐"
say "  │              FIRMIUS INSTALLER           │"
say "  └──────────────────────────────────────────┘"
info "Platform: $TARGET"
info "Destination: $INSTALL_DIR/firmius"
info "Downloading $ASSET..."

if ! curl --fail --location --silent --show-error --retry 3 --output "$ARCHIVE" "$BASE/$ASSET"; then
  fail "Could not download a release for $TARGET. Try --source or visit https://github.com/$REPO/releases."
fi

curl --fail --location --silent --show-error --retry 3 --output "$CHECKSUMS" "$BASE/SHA256SUMS" \
  || fail "Could not download SHA256SUMS; refusing an unverified install."
EXPECTED=$(awk -v file="$ASSET" '$2 == file || $2 == "*" file { print $1; exit }' "$CHECKSUMS")
[ -n "$EXPECTED" ] || fail "SHA256SUMS did not contain $ASSET; refusing an unverified install."
[ "${#EXPECTED}" -eq 64 ] || fail "SHA256SUMS contained a malformed checksum for $ASSET; refusing an unverified install."
case "$EXPECTED" in *[!A-Fa-f0-9]*) fail "SHA256SUMS contained a malformed checksum for $ASSET; refusing an unverified install." ;; esac
ACTUAL=$(shasum -a 256 "$ARCHIVE" 2>/dev/null | awk '{print $1}' || true)
if [ -z "$ACTUAL" ]; then ACTUAL=$(sha256sum "$ARCHIVE" 2>/dev/null | awk '{print $1}' || true); fi
[ -n "$ACTUAL" ] || fail "Neither shasum nor sha256sum is available to verify the release."
[ "$(printf '%s' "$EXPECTED" | tr 'A-F' 'a-f')" = "$(printf '%s' "$ACTUAL" | tr 'A-F' 'a-f')" ] || fail "Checksum verification failed."
info "Checksum verified."

mkdir -p "$TMP/unpacked" "$INSTALL_DIR"
case "$EXT" in
  tar.gz) tar -xzf "$ARCHIVE" -C "$TMP/unpacked" ;;
  zip)
    command -v unzip >/dev/null 2>&1 || fail "unzip is required to install the Windows archive."
    unzip -q "$ARCHIVE" -d "$TMP/unpacked" ;;
esac

BINARY=$(find "$TMP/unpacked" -type f \( -name firmius -o -name firmius.exe \) -print | head -n 1)
[ -n "$BINARY" ] || fail "The release archive did not contain a firmius binary."
SUFFIX=
case "$TARGET" in *windows*) SUFFIX=.exe ;; esac
DEST="$INSTALL_DIR/firmius$SUFFIX"
stop_running_daemon "$DEST"
if [ -e "$DEST" ]; then info "Existing install found; replacing it atomically."; else info "Creating a new install."; fi
STAGED="$INSTALL_DIR/.firmius.new.$$"
cp "$BINARY" "$STAGED"
chmod +x "$STAGED" 2>/dev/null || true
mv -f "$STAGED" "$DEST"

# Write metadata only after the binary has been successfully installed. A
# same-directory rename keeps readers from observing a partial JSON document.
MARKER_TMP="$INSTALL_DIR/.firmius-install.json.$$"
printf '{"channel":"release-script","repo":"%s","version":"%s"}\n' "$REPO" "$VERSION" > "$MARKER_TMP"
mv -f "$MARKER_TMP" "$INSTALL_DIR/firmius-install.json"

say ""
say "  ✓ Firmius installed successfully."
case ":${PATH:-}:" in
  *":$INSTALL_DIR:"*) ;;
  *)
    info "Add this directory to your PATH:"
    say "    export PATH=\"$INSTALL_DIR:\$PATH\""
    ;;
esac
say "  Run: firmius"
