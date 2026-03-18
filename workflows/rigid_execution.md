---
name: Rigid Execution
description: Execute an existing plan through bounded chunks, implementation, and review.
---
# Rigid Execution Workflow

This workflow executes an existing plan through explicit chunk ownership and review.

1. **Initialization (`lead`)**: The `lead` reads the active plan and selects ready chunks whose dependencies and priorities make them executable now.
2. **State Transition (`lead`)**: The `lead` keeps plan and chunk state current before dispatching work.
3. **Task Dispersal (`lead` -> `executor`)**: For each ready chunk, the `lead` assigns an `executor` with bounded instructions, scope limits, and explicit verification expectations.
4. **Implementation (`executor`)**: The `executor` implements one chunk only, verifies its changes, and reports implementation status, verification, blockers, and residual risk.
5. **Validation (`lead` -> `auditor`)**: The `lead` routes the returned chunk to `auditor` for evidence-first review against chunk intent and regression risk.
6. **Pass/Fail Routing**:
   - If the `auditor` finds the chunk acceptable, the `lead` advances the chunk state.
   - If the `auditor` finds issues, the `lead` refines the chunk, retries it, or reassigns it with explicit correction instructions.
7. **Completion**: The loop continues until the active plan is complete or a blocking decision is reached.
