# Memory benchmark plan

This is a measurement protocol, not a pre-baked marketing claim. It exists so
we can test whether Firmius beats competing harnesses (including JCODE) on the
same work.

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
| Firmius | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |
| Comparator | _pending_ | _pending_ | _pending_ | _pending_ | _pending_ |

Open an issue or PR with the raw measurements. Reproducible numbers are how we
turn “100x better” into something users can trust.