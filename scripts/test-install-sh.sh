#!/bin/sh

# No-network fixture verification for the Unix release installer.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d 2>/dev/null || mktemp -d -t firmius-installer-test)
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

FAKE_BIN="$WORK/bin"
FIXTURES="$WORK/fixtures"
mkdir -p "$FAKE_BIN" "$FIXTURES/payload"
REAL_MV=$(command -v mv)

cat > "$FIXTURES/payload/firmius" <<'EOF'
#!/bin/sh
printf '%s\n' fixture-firmius
EOF
chmod +x "$FIXTURES/payload/firmius"
tar -czf "$FIXTURES/archive.tar.gz" -C "$FIXTURES/payload" firmius

if command -v sha256sum >/dev/null 2>&1; then
  DIGEST=$(sha256sum "$FIXTURES/archive.tar.gz" | awk '{print $1}')
else
  DIGEST=$(shasum -a 256 "$FIXTURES/archive.tar.gz" | awk '{print $1}')
fi
ASSET=firmius-x86_64-unknown-linux-gnu.tar.gz
printf '%s  %s\n' "$DIGEST" "$ASSET" > "$FIXTURES/valid"
printf '%064d  other-asset.tar.gz\n' 0 > "$FIXTURES/missing-entry"
printf 'not-a-sha256  %s\n' "$ASSET" > "$FIXTURES/malformed-entry"
printf '%064d  %s\n' 0 "$ASSET" > "$FIXTURES/mismatch"

cat > "$FAKE_BIN/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
  -s) printf '%s\n' Linux ;;
  -m) printf '%s\n' x86_64 ;;
  *) exit 2 ;;
esac
EOF

cat > "$FAKE_BIN/curl" <<'EOF'
#!/bin/sh
set -eu
output=
url=
while [ "$#" -gt 0 ]; do
  case "$1" in
    --output) output=$2; shift 2 ;;
    *) url=$1; shift ;;
  esac
done
printf '%s\n' "$url" >> "$FIXTURE_CURL_LOG"
case "$url" in
  */SHA256SUMS)
    [ "$FIXTURE_CASE" != absent-checksum ] || exit 22
    cp "$FIXTURE_DIR/$FIXTURE_CASE" "$output"
    ;;
  *) cp "$FIXTURE_DIR/archive.tar.gz" "$output" ;;
esac
EOF

cat > "$FAKE_BIN/mv" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$FIXTURE_MV_LOG"
exec "$FIXTURE_REAL_MV" "$@"
EOF
chmod +x "$FAKE_BIN/uname" "$FAKE_BIN/curl" "$FAKE_BIN/mv"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_file_equals() {
  expected=$1
  file=$2
  actual=$(cat "$file")
  [ "$actual" = "$expected" ] || fail "$file changed: expected '$expected', got '$actual'"
}

run_installer() {
  fixture_case=$1
  destination=$2
  shift 2
  FIXTURE_CASE=$fixture_case \
  FIXTURE_DIR=$FIXTURES \
  FIXTURE_CURL_LOG=$WORK/curl.log \
  FIXTURE_MV_LOG=$WORK/mv.log \
  FIXTURE_REAL_MV=$REAL_MV \
  FIRMIUS_REPO=fixture/repo \
  FIRMIUS_VERSION=v1.2.3 \
  PATH="$FAKE_BIN:$PATH" \
    sh "$ROOT/install.sh" --dir "$destination" "$@" > "$WORK/output" 2>&1
}

for fixture_case in absent-checksum missing-entry malformed-entry mismatch; do
  destination="$WORK/dest-$fixture_case"
  mkdir -p "$destination"
  printf '%s\n' old-binary > "$destination/firmius"
  printf '%s\n' old-marker > "$destination/firmius-install.json"
  if run_installer "$fixture_case" "$destination"; then
    fail "$fixture_case unexpectedly installed"
  fi
  assert_file_equals old-binary "$destination/firmius"
  assert_file_equals old-marker "$destination/firmius-install.json"
  [ -z "$(find "$destination" -name '.firmius.new.*' -o -name '.firmius-install.json.*')" ] \
    || fail "$fixture_case left a staged file"
done

destination="$WORK/custom-destination"
run_installer valid "$destination" || {
  cat "$WORK/output" >&2
  fail "valid fixture did not install"
}
[ -x "$destination/firmius" ] || fail "installed binary is not executable"
cmp -s "$FIXTURES/payload/firmius" "$destination/firmius" \
  || fail "installed binary does not match the verified fixture"
assert_file_equals '{"channel":"release-script","repo":"fixture/repo","version":"v1.2.3"}' \
  "$destination/firmius-install.json"
[ -z "$(find "$destination" -name '.firmius.new.*' -o -name '.firmius-install.json.*')" ] \
  || fail "successful install left a staged file"
grep -F '.firmius.new.' "$WORK/mv.log" | grep -F " $destination/firmius" >/dev/null \
  || fail "binary was not committed with a same-directory staged rename"
grep -F '.firmius-install.json.' "$WORK/mv.log" | grep -F " $destination/firmius-install.json" >/dev/null \
  || fail "marker was not committed with a same-directory staged rename"

# Unsafe repo/version values must fail before fake curl observes a request.
: > "$WORK/curl.log"
for invalid in 'repo:owner/repo/extra' 'repo:owner"/repo' 'version:v1beta' 'version:v1.2/3'; do
  kind=${invalid%%:*}
  value=${invalid#*:}
  if [ "$kind" = repo ]; then
    if FIRMIUS_REPO=$value PATH="$FAKE_BIN:$PATH" FIXTURE_CURL_LOG=$WORK/curl.log \
      sh "$ROOT/install.sh" --dir "$WORK/invalid" > "$WORK/output" 2>&1; then
      fail "invalid repository '$value' was accepted"
    fi
  else
    if FIRMIUS_VERSION=$value PATH="$FAKE_BIN:$PATH" FIXTURE_CURL_LOG=$WORK/curl.log \
      sh "$ROOT/install.sh" --dir "$WORK/invalid" > "$WORK/output" 2>&1; then
      fail "invalid version '$value' was accepted"
    fi
  fi
done
if PATH="$FAKE_BIN:$PATH" FIXTURE_CURL_LOG=$WORK/curl.log \
  sh "$ROOT/install.sh" --dir "$WORK/invalid" --version v1beta > "$WORK/output" 2>&1; then
  fail "invalid --version argument was accepted"
fi
[ ! -s "$WORK/curl.log" ] || fail "invalid input triggered a network request"

printf '%s\n' 'install.sh fixture verification passed'