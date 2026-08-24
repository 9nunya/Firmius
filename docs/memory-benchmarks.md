# Memory benchmark plan

This is a measurement protocol, not a pre-baked marketing claim. It exists so
we can test whether Firmius beats competing harnesses (including JCODE) on the
same work.

## Latest local run (2026-08-19)

Three repetitions were run on the same machine and workload (`Reply with
exactly: benchmark-ok`). Firmius was launched in a detached tmux pseudo-
terminal (the normal TUI startup path), the prompt was sent through the TUI,
and peak RSS was sampled for 60 seconds. Jcode was run non-interactively with
the same prompt under `/usr/bin/time -l`, requesting `gpt-5.6-luna`.

| Harness | Samples | Peak RSS (min / mean / max) | Actual model |
|---|---:|---:|---|
| Firmius TUI | 3 | 21,536 / 22,219 / 22,672 KiB | codex/gpt-5.6-luna configured in TUI |
| Jcode | 3 | 284,426,240 / 288,134,485 / 291,094,528 bytes | claude-opus-4-8 (provider ignored requested name) |

This is an observed process-RSS result, not a controlled model comparison:
the installed Jcode provider resolved to `claude-opus-4-8` despite the requested
model name. Firmius's peak is about 21.7 MiB; Jcode's is about 274.8 MiB, or
roughly 12.7x higher in this run. Repeat after both harnesses can select the
same model and provider before claiming superiority.

## Record

- commit, Rust/toolchain, OS, CPU, RAM, provider/model;
- workload and repository size;
- cold-start and steady-state peak RSS (bytes), wall time, and output quality;
- context tokens before/after compaction and artifact sizes;
- at least 5 repetitions, with median and p95.

## Suggested run

Build once with `cargo build --release`, run an identical scripted workload
under `/usr/bin/time -v` (or `/usr/bin/time -l` on macOS), and save the raw
output. Repeat the exact workload against each comparator. Never call a result
“best ever” unless the workload, versions, and raw data are published.

## Results table

| Harness | Workload | Peak RSS | Median time | Commit | Notes |
|---|---|---:|---:|---|---|
| Firmius TUI | benchmark-ok | 22,219 KiB mean | measured | local run | tmux TUI prompt |
| Jcode | benchmark-ok | 288,134,485 bytes mean | measured | local run | resolved Claude model |

Open an issue or PR with the raw measurements. Reproducible numbers are how we
turn “100x better” into something users can trust.