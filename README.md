# Firmius

<p align="center">
  <strong>A local-first, multi-provider AI coding workspace for the terminal.</strong><br>
  One TUI for agents, tools, sessions, personas, MCP servers, and provider accounts.
</p>

<p align="center">
  <a href="https://github.com/9nunya/Firmius/releases"><img src="https://img.shields.io/github/v/release/9nunya/Firmius?display_name=tag&sort=semver" alt="Latest release"></a>
  <a href="https://github.com/9nunya/Firmius/actions/workflows/release.yml"><img src="https://github.com/9nunya/Firmius/actions/workflows/release.yml/badge.svg" alt="Release workflow"></a>
</p>

Firmius is a Rust terminal application for working with AI models while keeping your sessions, provider accounts, personas, and settings on your machine. It supports OpenAI-compatible APIs, Anthropic, subscription-backed account kinds, and MCP tool servers.

## Install

### macOS and Linux

The installer detects your operating system and CPU, downloads the matching release archive, verifies its SHA-256 checksum when available, and places `firmius` in `~/.local/bin`:

```sh
curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh
```

Install a specific release or choose another directory:

```sh
curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh -s -- --version v0.1.0 --dir "$HOME/.local/bin"
```

If a prebuilt artifact is not available for your machine, build directly from the repository:

```sh
curl -fsSL https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.sh | sh -s -- --source
```

The installer never needs `sudo` by default. Add `~/.local/bin` to your `PATH` if your shell does not already include it:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

### Windows

From PowerShell:

```powershell
irm https://raw.githubusercontent.com/9nunya/Firmius/refs/heads/master/install.ps1 | iex
```

The Windows installer adds `%USERPROFILE%\.local\bin` to your user `PATH`. Open a new terminal after installation.

### Build with Cargo

Rust 1.85 or newer is required because Firmius uses the Rust 2024 edition:

```sh
git clone https://github.com/9nunya/Firmius.git
cd Firmius
cargo install --locked --path crates/firmius
```

For a development build:

```sh
cargo run --package firmius
```

## Quick start

Firmius can bootstrap its first provider from an environment variable. For OpenAI-compatible APIs:

```sh
export OPENAI_API_KEY="your-key"
firmius
```

For Anthropic:

```sh
export ANTHROPIC_API_KEY="your-key"
firmius
```

You can also select a model, provider, or compatible endpoint:

```sh
export FIRMIUS_MODEL="gpt-4o-mini"
export FIRMIUS_PROVIDER="openai"
export FIRMIUS_BASE_URL="https://api.openai.com/v1"
firmius
```

On first launch, use the TUI settings and account screens to configure providers, models, personas, retry behavior, and MCP servers. Credentials and sessions are persisted under `~/.firmius`.

Useful non-interactive options:

```sh
firmius --list-sessions
firmius --resume SESSION_ID
```

## What is included

- **Terminal UI:** A focused workspace for conversations, agents, streaming responses, and tool activity.
- **Built-in tools:** Read, list, edit, grep, glob, bash, and agent delegation tools.
- **Multiple providers:** OpenAI-compatible endpoints, Anthropic, Codex, Cline Pass, OpenCode Go, and Alibaba Token Plan account kinds.
- **Personas:** General, coder, lead, and reviewer defaults that can be customized locally.
- **Sessions:** Persist, list, and resume work without losing context.
- **MCP:** Load enabled Model Context Protocol servers and expose their tools to agents.
- **Resilience:** Configurable retries, backoff, account switching, and provider-specific policies.
- **Local-first state:** Configuration, account records, settings, and sessions stay in `~/.firmius`.

## Environment variables

| Variable | Purpose |
| --- | --- |
| `OPENAI_API_KEY` | Bootstrap an OpenAI-compatible provider |
| `ANTHROPIC_API_KEY` | Bootstrap an Anthropic provider |
| `FIRMIUS_BASE_URL` | Override the base URL for the provider being bootstrapped |
| `FIRMIUS_PROVIDER` | Select a configured provider |
| `FIRMIUS_MODEL` | Select the model, defaulting to `gpt-4o-mini` |
| `CLINE_API_KEY` | Bootstrap a Cline Pass account |
| `OPENCODE_API_KEY` | Bootstrap an OpenCode Go account |
| `ALIBABA_TOKEN_PLAN_API_KEY` | Bootstrap an Alibaba Token Plan account |
| `ALIBABA_REGION` | Alibaba region, defaulting to `international` |

The installer also accepts `FIRMIUS_VERSION`, `FIRMIUS_INSTALL_DIR`, and `FIRMIUS_REPO` for automation and mirrors.

## Release artifacts

The release workflow is triggered by a **Git tag**, not by the commit message. To publish a release, create and push a semantic version tag:

```sh
git tag v0.1.0
git push origin v0.1.0
```

You can also run it manually from the GitHub Actions tab with **Run workflow**, but tagged releases are preferred because the assets are attached to that version tag. A commit whose message contains `v` alone will not trigger the release workflow.

Pushing a tag such as `v0.1.0` runs the release workflow and publishes archives for:

- `x86_64-unknown-linux-gnu`
- `x86_64-apple-darwin`
- `aarch64-apple-darwin`
- `x86_64-pc-windows-msvc`

Each release includes a `SHA256SUMS` file. Linux ARM and other unsupported targets can use the installer's `--source` fallback or build with Cargo.

## Development

Run the formatter and test suite from the repository root:

```sh
cargo fmt --all -- --check
cargo test --workspace
```

Build only the application binary:

```sh
cargo build --package firmius
```

The workspace is organized as:

```text
crates/core     Shared providers, agents, tools, persistence, MCP, and settings
crates/firmius  Terminal application and TUI
crates/audits   Audit utilities
```

## Security and privacy

Treat API keys as secrets. Prefer environment variables or the built-in account manager, and do not commit `~/.firmius` or exported credentials. Firmius stores local state in your home directory and uses TLS for its built-in HTTP clients.

Please report security issues privately through the repository's security contact rather than opening a public issue with credentials or exploit details.

## License

See [LICENSE](LICENSE) for the project license.
