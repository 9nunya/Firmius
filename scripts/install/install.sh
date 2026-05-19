#!/usr/bin/env bash
# Firmius installer for Linux and macOS.
#
# Lays out:
#   <prefix>/bin/firmius                  (or firmiusd)
#   <prefix>/share/firmius/themes
#   <user_data>/.firmius/{prompts,workflows,hinting,themes}
#
# Defaults:
#   prefix     = ${PREFIX:-/usr/local}      (use --prefix to override)
#   user_data  = ${HOME}/.firmius            (do not override; firmius assumes this)
#
# Usage:
#   ./install.sh                # interactive defaults
#   PREFIX=$HOME/.local ./install.sh
#   ./install.sh --prefix /opt/firmius
#   ./install.sh --uninstall

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
ACTION="install"

usage() {
  cat <<EOF
Firmius installer

Usage: $0 [OPTIONS]

Options:
  --prefix DIR   Install prefix (default: \$PREFIX or /usr/local)
  --uninstall    Remove the installed binaries
  -h, --help     Show this help

Environment:
  PREFIX         Same as --prefix
  FIRMIUS_HOME   Override per-user data root (default: \$HOME/.firmius)
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --prefix)
      PREFIX="$2"
      shift 2
      ;;
    --prefix=*)
      PREFIX="${1#--prefix=}"
      shift
      ;;
    --uninstall)
      ACTION="uninstall"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

USER_DATA="${FIRMIUS_HOME:-$HOME/.firmius}"
BIN_DIR="$PREFIX/bin"
DATA_DIR="$PREFIX/share/firmius"

# Choose sudo if writing to a system path and not running as root.
sudo_cmd=""
if [ "$ACTION" = "install" ]; then
  # Walk up the BIN_DIR path until we find an existing ancestor; if it's not
  # writable and we aren't root, escalate via sudo.
  probe="$BIN_DIR"
  while [ ! -e "$probe" ]; do
    next="$(dirname "$probe")"
    if [ "$next" = "$probe" ]; then
      break
    fi
    probe="$next"
  done
  if [ ! -w "$probe" ] && [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
      sudo_cmd="sudo"
      echo "==> $probe is not writable; using sudo"
    else
      echo "error: $probe is not writable and sudo is not available" >&2
      echo "       try: PREFIX=\$HOME/.local $0" >&2
      exit 1
    fi
  fi
fi

if [ "$ACTION" = "uninstall" ]; then
  echo "==> Uninstalling firmius from $PREFIX"
  $sudo_cmd rm -f "$BIN_DIR/firmius" "$BIN_DIR/firmiusd"
  $sudo_cmd rm -rf "$DATA_DIR"
  echo "    User data at $USER_DATA was NOT removed (delete manually if desired)."
  echo "==> Done."
  exit 0
fi

# Verify the source files are present.
if [ ! -x "$SCRIPT_DIR/bin/firmius" ]; then
  echo "error: firmius binary not found at $SCRIPT_DIR/bin/firmius" >&2
  exit 1
fi

echo "==> Installing firmius"
echo "    prefix:    $PREFIX"
echo "    user data: $USER_DATA"

$sudo_cmd mkdir -p "$BIN_DIR" "$DATA_DIR"
$sudo_cmd cp "$SCRIPT_DIR/bin/firmius" "$BIN_DIR/firmius"
$sudo_cmd chmod 755 "$BIN_DIR/firmius"
if [ -f "$SCRIPT_DIR/bin/firmiusd" ]; then
  $sudo_cmd cp "$SCRIPT_DIR/bin/firmiusd" "$BIN_DIR/firmiusd"
  $sudo_cmd chmod 755 "$BIN_DIR/firmiusd"
fi

# Bundled themes go to the system data dir so multiple users can share them.
if [ -d "$SCRIPT_DIR/share/firmius" ]; then
  $sudo_cmd cp -R "$SCRIPT_DIR/share/firmius/." "$DATA_DIR/"
fi

# Per-user assets are placed in the user data dir, but only if not already present.
mkdir -p "$USER_DATA"
for sub in prompts workflows hinting themes; do
  if [ -d "$SCRIPT_DIR/$sub" ] && [ ! -d "$USER_DATA/$sub" ]; then
    cp -R "$SCRIPT_DIR/$sub" "$USER_DATA/$sub"
    echo "    seeded $USER_DATA/$sub"
  fi
done

echo "==> Done."
echo "    Run: firmius"
