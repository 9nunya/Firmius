---
description: The Firmius Ideation Loop
---
# Ideation Workflow

This workflow transforms a vague user objective into a strict XML roadmap.

1. **Initialization**: The human user provides a high-level goal or objective.
2. **Context Gathering (`brainstormer`)**: The `brainstormer` agent acts as the technical lead. It reads the repository structure using its exploration tools (`glob`, `file_read`). It asks 2-3 highly targeted questions to the human to clarify constraints.
3. **Drafting (`brainstormer`)**: Once alignment is reached, the `brainstormer` outputs a conversational but thorough `DRAFT_PLAN.md` documenting the agreed-upon approach, and emits `[BRAINSTORM_COMPLETE]`.
4. **Handoff**: Execution is passed to the `planner`.
5. **Formalization (`planner`)**: The `planner` agent ingests `DRAFT_PLAN.md` and generates a rigid `ROADMAP.md` filled with explicit XML gates (`<phase>`, `<plan>`, `<task>`, `<quality_gate>`). It emits `[PLANNING_COMPLETE]`.
