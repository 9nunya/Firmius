# Firmius

**The terminal-native AI coding harness for work that has to finish.**

Firmius gives agents a real repository, real tools, a durable daemon, and an
operator-visible TUI. It is built for execution—not chat theater.

## What makes Firmius OP

- **Durable work:** sessions, goals, todos, work graphs, runs, and results
  survive disconnects and restarts.
- **A real team loop:** delegate to coder, reviewer, and general agents; run
  independent branches in parallel; bind results; retry bounded failures.
- **Proof over vibes:** verification gates, independent review, evidence,
  annotations, explicit outcomes, and quality digests make “done” inspectable.
- **Daemon-backed TUI:** the terminal client reconnects, reconciles snapshots,
  keeps accepted work running, and shows authoritative state instead of
  guessing from transcript text.
- **Safe execution:** permissions, leases, authenticated local IPC, scoped
  tools, SSH workspace targeting, edit history, and focused-agent ownership
  keep consequential work bounded.
- **Context that holds up:** compaction, budgets, durable memory, and typed
  artifacts keep long sessions useful without dragging every old token forward.

## Start in two commands

```sh
cargo build --release
cargo run --release
```

Installed command:

```sh
firmius --help
firmius doctor
firmius daemon
```

The TUI starts and manages its local daemon automatically. Use
`firmius --ssh <host> <absolute-directory>` for a remote workspace. Use
`firmius ssh-hosts` to inspect and save SSH targets.

## Ship a real run

Ask for an outcome, not a vague chat:

```text
Inspect the repository. Plan the change with acceptance criteria. Delegate
implementation and review separately. Run the checks. Show the evidence.
```

For a durable goal from a shell:

```sh
firmius goal create "Ship the next release"
firmius goal list
```

For an immediate provider-backed run:

```sh
FIRMIUS_PROVIDER=openai FIRMIUS_MODEL=gpt-4o-mini \
  firmius goal create "Ship the next release"
```

Useful TUI commands include `/help`, `/onboarding`, `/workflow`, `/ssh`,
`/undo`, `/redo`, and `/edit-history`. The daemon owns the durable state; the
TUI is the fast operator surface.

## Install a release

macOS/Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1 | iex
```

Release archives are checksum-verified before installation. The installer
records its channel as metadata; that marker is not authentication.

## Read the docs

- [Install and first run](docs/wiki/getting-started.md)
- [Core concepts](docs/wiki/concepts.md)
- [Workflows and delegation](docs/wiki/workflows.md)
- [Agent workflows and prompt inspection](docs/wiki/agent-workflows.md)
- [Memory and context](docs/wiki/memory.md)
- [Troubleshooting](docs/wiki/troubleshooting.md)
- [Contributing](CONTRIBUTING.md)

## Project boundary

This release is the Firmius CLI/TUI, daemon, protocol, client, and core
runtime.

## License

See the repository license for terms.
