---
name: Systematic Debug
description: Debug through bounded hypotheses, isolated execution, and review.
---
# Systematic Debug Workflow

This workflow is used when a bug is reported or a Quality Gate persistently fails. It enforces the scientific method.

1. **Triaging (`lead` -> `scout`)**: The `lead` dispatches `scout` to define the error surface by gathering logs, stack traces, reproduction steps, and relevant file contexts.
2. **State Tracking (`lead`)**: The `lead` creates or updates a debug plan and the chunk that owns the current hypothesis.
3. **The Scientific Loop**:
   - **Hypothesis Definition (`lead`)**: The `lead` writes a clear root-cause hypothesis and the signal that would confirm or refute it.
   - **Isolation (`lead` -> `executor`)**: An `executor` is assigned a bounded chunk that changes exactly one variable, path, or module needed to test the hypothesis.
   - **Verification (`lead` -> `auditor`)**: The `auditor` or the `executor` runs the reproduction steps or tests needed to judge the hypothesis.
   - **Observation (`lead`)**: The `lead` records the outcome. If the hypothesis is refuted, the `lead` revises the plan and forms the next hypothesis.
4. **Resolution**: Once a hypothesis is confirmed, the `lead` assigns an `executor` to produce the clean fix and routes it through `auditor` to validate the broader regression surface.
