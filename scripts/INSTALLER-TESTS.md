# Installer regression checks

Run `sh scripts/test-install-sh.sh` on macOS/Linux. The suite uses real archive
extraction, checksums and installation with local download fixtures. It checks
CLI and daemon installation, rejection of old daemon-less
archives without replacement, and source-install daemon failures.

On Windows run
`powershell -NoProfile -File scripts/test-install-ps1.ps1`. This uses real ZIP
extraction and Windows file replacement with local download fixtures. It restores
the user PATH after testing. Detached replacement under executable locks still
requires a Windows manual integration check: keep an installed executable open,
run the installer, expect exit 2 and pending state, release the executable, and
verify all three binaries plus metadata are finalized and the state removed.

Release archives must contain both `firmius` and `firmiusd` (with `.exe` on
Windows). Older CLI-only archives are explicitly rejected before replacement;
select a newer release or build from source instead. Installers include only the CLI and daemon.

The Unix installer's `--source` mode installs the daemon from the
`firmius-service` package as well as the CLI, into Cargo's bin directory. For a
local checkout, use both commands (installing only the CLI is insufficient):

```sh
cargo install --locked --path crates/service --bin firmiusd
cargo install --locked --path crates/firmius --bin firmius
```

Ensure `${CARGO_HOME:-$HOME/.cargo}/bin` is on PATH.