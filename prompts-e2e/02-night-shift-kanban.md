Create a greenfield Python project in /work/night-shift-kanban. This workspace is inside the Firmius debugging container; the finished project can later be synced to /mnt/SHIT/Projects/night-shift-kanban on the host.

Act as Firmius lead. This is a planner-pipeline task, not a quick direct commit. Use discovery, planner, plan_checker, then commit all chunks before any execution. After commitment, use executors and workers to finish the plan.

Build an offline kanban and shift handoff manager with these requirements:
- Python 3.11, standard library first.
- CLI app with boards, columns, task priorities, and handoff notes.
- Local JSON or sqlite persistence.
- Report command that prints unresolved blockers for the next shift.
- Sample seed data.
- Unit tests covering persistence and reporting.
- README with usage examples.

Quality gates:
- Planner artifact and plan_checker critique artifact must both exist.
- At least 4 plan chunks.
- Final verification must include python -m unittest and one end-to-end scripted usage example.
