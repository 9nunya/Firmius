---
description: The Firmius Systematic Debug Loop
---
# Systematic Debug Workflow

This workflow is used when a bug is reported or a Quality Gate persistently fails. It enforces the scientific method.

1. **Triaging (`coordinator` -> `scout`)**: The `coordinator` spawns a `scout` agent to define the error surface, gathering logs, stack traces, and relevant file contexts.
2. **State Tracking**: The `coordinator` initializes a persistent state file (e.g., `.planning/debug_SESSION.md`).
3. **The Scientific Loop**:
   - **Hypothesis Definition**: A clear hypothesis regarding the root cause is written to the state file.
   - **Isolation (`builder`)**: A `builder` agent is spawned to modify exactly ONE variable/module to test the hypothesis.
   - **Verification (`reviewer`)**: A `reviewer` runs the test suite or reproduction steps.
   - **Observation**: The outcome is recorded in the state file. If the hypothesis is refuted, revert changes and form a new hypothesis.
4. **Resolution**: Once a hypothesis is confirmed, the `builder` creates the permanent, clean fix. The `reviewer` validates it against the broader test suite to ensure no regressions.
