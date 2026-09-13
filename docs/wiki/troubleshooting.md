# Troubleshooting

Start with `firmius doctor` when installation or update behavior is surprising.
It reports the running executable, detected install channel, marker status, and
safe update method. A missing marker on an older release is not automatically
an error: Firmius falls back to conservative path/source detection and refuses
to self-replace when ownership cannot be established.

On Windows, an executable that is still in use may require deferred
replacement. The installer records `firmius-update-state.json` with a `pending`
status and exits with status 2. This means replacement is pending, **not** that
the update succeeded; exit all Firmius processes, then inspect the state file
or rerun the installer. A helper failure is recorded as `failed` in that file.

If first-run onboarding cannot save settings, it remains open and displays the
save error rather than marking onboarding complete. If
`firmius --reset-onboarding` encounters malformed settings, reset aborts and
leaves the malformed file untouched for diagnosis or repair.

For source failures, start with `cargo check --workspace` and
`cargo test --workspace`. Capture the first error, not just the final cascade.
For a stuck run, inspect graph status, pending gates, worker ownership, and
persisted session state. For context issues, reduce artifact size, set an
explicit budget, and compare a fresh session with a resumed one. Redact API
keys and home-directory paths before sharing `firmius doctor` output.

## Daemon connectivity

Interactive TUI sessions use the local daemon embedded in the `firmius`
executable. A separately built `firmiusd` binary is also available for source
deployments, but the release installer does not require a second executable.
The daemon publishes endpoint metadata, including a random bearer token, under
Firmius's data directory and keeps accepted turns running independently of a
client window. After a transport failure the TUI retains its last snapshot,
retries the endpoint, reattaches the session, and refreshes authoritative
state. Event gaps likewise request a snapshot instead of guessing missing
transcript or work state.

The endpoint is protected by an exclusive lease and is intended for local use,
not public network exposure. Do not copy the endpoint file or expose its port.
The daemon removes endpoint metadata during normal shutdown; remove a stale
file only after confirming that no daemon process is running.

`firmius daemon` runs until explicitly stopped or until an idle timeout is
configured. Intentional shutdown is not treated as a reconnectable crash.
Accepted turns may finish after a client disconnect, with persistence at turn
completion and daemon shutdown; a process or machine failure can still lose
the latest in-flight updates.
