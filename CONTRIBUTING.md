# Contributing to Firmius

Welcome. Firmius gets better when real users report the sharp edges. I am not a
madman; I just want excellent bug reports.

## Issues and feature requests

Before opening an issue, search existing reports. For bugs, include:

1. what you ran and what you expected;
2. what happened, including the complete error (redact secrets);
3. OS, Rust version, Firmius commit/version, and provider;
4. the smallest reproduction you can share.

For features, explain the user problem, proposed workflow, alternatives, and
whether it affects persistence, authorization, or memory usage.

## Pull requests

Keep PRs focused. Add or update tests and docs, run `cargo fmt --check`,
`cargo check --workspace`, and relevant `cargo test --workspace` tests. Explain
tradeoffs in the PR description, especially around context retention and
concurrency. Do not commit credentials, tokens, private prompts, or generated
build artifacts.

Performance PRs must include before/after commands, workload, hardware, sample
count, and peak RSS (not a hand-picked number).