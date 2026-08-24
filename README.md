# Firmius

**The terminal-native AI coding harness.**

Firmius is not another chat window wearing a developer costume. It is a
durable, scriptable harness for getting real work done in a repository: search
files, edit code, delegate parallel work, and keep the whole operation
auditable. I am not a madman. Firmius is simply what happens when the terminal
gets tired of being underestimated.

## Why Firmius

- **Work graphs:** plan DAGs, run managed workflows, bind predecessor results,
  retry bounded failures, and inspect live runs.
- **Collaboration:** delegate to focused coder, reviewer, and general agents;
  message parents, siblings, the fleet, or a task.
- **Durable sessions:** persistence, resume reconciliation, ownership, and
  mailbox delivery keep long jobs understandable after interruptions.
- **Verification:** gates, independent reviewers, annotations, quality
  digests, and explicit outcomes make “done” more than a vibes-based status.
- **Terminal-first UX:** TUI views, clipboard support, settings, run context,
  and ordinary CLI workflows.
- **Memory discipline:** context budgets, compaction projections, artifacts,
  and durable snapshots are designed to keep useful state without dragging an
  entire conversation forever.

## Quick start

```sh
cargo build --release
cargo run --release
```

Read the [user wiki](docs/wiki/README.md) for installation, configuration,
workflows, and contribution guides.

## Memory claims, measured properly

We want to earn the claim that Firmius is exceptionally memory-efficient—not
just shout it. The repository includes a reproducible measurement plan in
[`docs/memory-benchmarks.md`](docs/memory-benchmarks.md). Until those runs are
performed on a declared machine and workload, comparisons to JCODE or any
other harness are **hypotheses, not results**. Run the benchmark, commit the
numbers, and let users verify the flex.

## Help improve the harness

Issues, bug reports, feature requests, benchmark workloads, documentation
fixes, and pull requests are welcome. Please include reproduction steps,
platform, Firmius version, and (for performance reports) the workload and
measurement command. See [CONTRIBUTING.md](CONTRIBUTING.md) and the
[contribution guide](docs/wiki/contributing.md).

## License

See the repository license for terms.