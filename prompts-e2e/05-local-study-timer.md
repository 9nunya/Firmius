Create a greenfield Python project in /work/local-study-timer. This workspace is inside the Firmius debugging container; the finished project can later be synced to /mnt/SHIT/Projects/local-study-timer on the host.

This task must exercise Firmius orchestration, so behave as the lead and do a real discovery -> planner -> plan_checker -> lead commit -> executor/worker execution pipeline. Do not shortcut planning.

Build a local study timer and session tracker with:
- Python 3.11 standard library.
- Timer profiles, study sessions, break sessions, and daily summary reporting.
- Persistence using sqlite3.
- CLI commands for start-session, stop-session, daily-report, and seed-demo.
- Unit tests with unittest.
- README with setup and demo usage.

Acceptance:
- planner and plan_checker artifact trail exists
- at least 4 committed plan chunks
- python -m unittest passes
- scripted demo run succeeds
