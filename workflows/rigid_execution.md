---
description: The Firmius Rigid Execution Loop
---
# Rigid Execution Workflow

This workflow executes a predefined `ROADMAP.md` with zero Hallucinations and strict Quality Gates.

1. **Initialization**: The `coordinator` agent reads the `ROADMAP.md` and identifies the first `<plan status="pending">`.
2. **State Transition**: The `coordinator` marks the plan as `status="in_progress"` in the XML.
3. **Task Dispersal (`coordinator` -> `builder`)**: For each `<task status="pending">` whose dependencies are met, the `coordinator` spawns a `builder` agent, providing it ONLY the `<files_to_edit>` and `<constraints>`.
4. **Implementation (`builder`)**: The `builder` implements the code, validates syntax compilation, and returns `[BUILD_COMPLETE]`.
5. **Validation (`coordinator` -> `reviewer`)**: The `coordinator` spawns a `reviewer` agent, passing the `builder`'s diff and the physical `<quality_gate>` required by the task.
6. **Pass/Fail Routing**: 
   - If `reviewer` returns `[REVIEW_RESULT] Passed`, the task is marked `status="done"` in the `ROADMAP.md`.
   - If `reviewer` returns `[REVIEW_RESULT] Failed: <reason>`, the `coordinator` respawns the `builder` with the explicit fail instructions.
7. **Completion**: Loop continues until all `<phase>` elements are `done`. The `coordinator` emits `[COORDINATION_COMPLETE]`.
