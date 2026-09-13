# Getting started

Firmius is a CLI/TUI plus a local daemon. The TUI is the operator surface; the
daemon owns accepted work and durable state.

## Install

Install the latest checksummed GitHub release on macOS or Linux:

```sh
curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh
```

On Windows PowerShell:

```powershell
irm https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1 | iex
```

The installers require the release checksum manifest, verify the archive, and
write a small `firmius-install.json` marker beside the executable. That marker
is metadata used to detect a claimed install channel; it is **not**
authentication. Firmius validates its fields and combines them with
conservative path checks rather than trusting it as authority.

## Build from source

Install a current Rust toolchain, then run:

```sh
cargo build --release
cargo run --release
```

## First run

The interactive TUI opens a short getting-started launchpad once. It shows the
detected install situation and whether a provider is connected, then offers to:

- connect or change a model provider;
- load a 30-second workflow tour; or
- start with a blank composer.

`Esc` dismisses the launchpad and remembers the choice—it will not nag on every
launch. Reopen it at any time with `/onboarding`. For demos and support, start
Firmius with `--reset-onboarding` to show it again on the next interactive
welcome screen. Resumed sessions and non-interactive commands are never blocked
by onboarding. If saving the choice fails, the launchpad stays open with an
error instead of pretending setup completed. Reset also aborts without
overwriting the file when existing settings are malformed.

When a session opens a repository, Firmius also loads project-local operating
instructions from `AGENTS.md`, `CLAUDE.md`, and `.firmius/instructions.md` in
the directory hierarchy. More specific files are applied after parent files.
They are shown to the model as repository policy, while permissions and tool
scopes remain enforced by the runtime.

## Understand and update the current install

Before updating, inspect what Firmius detected:

```sh
firmius doctor
```

The diagnostic reports the installation channel plus runtime data storage,
daemon endpoint metadata, and configured provider accounts. A clean install is
expected to say that storage and providers are not initialized; it should not
silently look ready when the daemon or credentials are missing.

For portable or service-managed installs, set `FIRMIUS_DATA_DIR` to move the
entire Firmius state root. The daemon lease, sessions, accounts, settings,
permissions, MCP configuration, and personas all follow that same directory.

`firmius update` follows the owning channel rather than blindly overwriting the
current executable. A valid marker can identify an official release-installer
or official Cargo Git install, but remains metadata rather than authentication.
Before running its fixed official source, Firmius requires an interactive
terminal and the exact confirmation `update Firmius from 9nunya/Firmius`;
non-interactive use or any mismatch refuses before launching a process or
making a network request. There is intentionally no `--yes` bypass. Homebrew,
Nix, Scoop, Chocolatey, Snap, system
package, and source channels receive an advisory package/build command instead
of self-replacement. Unknown, standalone-without-valid-marker, and Cargo
installs whose source is unknown are refused safely and no files are changed.

Use `firmius update-check` for a read-only check against the official latest
release. It reports the current and latest versions and never stops the daemon
or launches an updater.

The autonomous goal CLI starts the local daemon when needed, just like the TUI:

```sh
firmius goal create "Ship the next release"
firmius goal list
```

You do not need to run `firmius daemon` in another terminal first. To dispatch
the goal immediately from a non-interactive shell, also select a configured
provider (and optionally a model):

```sh
FIRMIUS_PROVIDER=openai FIRMIUS_MODEL=gpt-4o-mini \
  firmius goal create "Ship the next release"
```

Without `FIRMIUS_PROVIDER`, the command still records a durable proposed goal;
open Firmius to attach it to an agent and run it. The daemon owns the profile
lease, so a second Firmius process cannot silently start a competing runtime.

Because the CLI and daemon share the installed executable, an update first
asks the authenticated local daemon to shut down and waits for its profile
lease to be released. If the daemon is locked but its endpoint cannot be
contacted, the update aborts rather than replacing a binary underneath an
unknown running runtime. A subsequent Firmius launch starts the new daemon.

Firmius is not published on crates.io. Build from source or use a GitHub
release archive.

Use the repository's configuration and provider documentation for credentials;
never paste secrets into issues or prompts. Start with a small repository and
ask for a plan before allowing edits.

## A safe first session

To start against an SSH target, run `firmius --ssh <alias-or-host> <absolute-remote-directory>` (for example, `/srv/project`). Relative directories are rejected before startup so the process and file tools cannot silently disagree about the workspace. Use `firmius ssh-hosts` to inspect concrete aliases from `~/.ssh/config` (including nested `Include` files) plus direct hosts found in `~/.ssh/known_hosts` before connecting. The selected remote directory is used for both process execution and file tools.

From an active daemon-backed TUI session, `/ssh <alias> <absolute-dir>` saves
the current session and opens the same kind of remote workspace in place. Quote
paths containing spaces, for example `/ssh build "/srv/my project"`.

To remember a discovered target and its usual workspace, use:

```sh
firmius ssh-hosts add build /srv/project
firmius ssh-hosts
firmius ssh-hosts open build
firmius ssh-hosts remove build
```

Saved targets are convenience metadata; SSH config, keys, and host verification
remain owned by the system SSH client.

Inside the TUI, `/ssh-add <alias> <absolute-dir>` saves the same target and
`/ssh-hosts` marks saved entries with their default directory.

File edits are tracked per agent. In the TUI, `/undo`, `/redo`, and `/edit-history` operate on the focused agent and use the same conflict checks as the model-facing `undo` tool; a sibling agent's newer edit is never silently overwritten.

1. Ask Firmius to inspect the repository and summarize it.
2. Ask for a plan with explicit acceptance criteria.
3. Delegate implementation and review separately.
4. Run tests and inspect the diff yourself.
5. Export or preserve the session when the work matters.
