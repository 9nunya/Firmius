# Troubleshooting

Start with `cargo check --workspace` and `cargo test --workspace`. Capture the
first error, not just the final cascade. For a stuck run, inspect graph status,
pending gates, worker ownership, and persisted session state. For context
issues, reduce artifact size, set an explicit budget, and compare a fresh
session with a resumed one. Redact API keys before sharing logs.