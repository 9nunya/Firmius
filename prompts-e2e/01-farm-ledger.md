Build a greenfield Python 3.11 farm ledger app in /work/farm-ledger. This workspace is inside the Firmius debugging container; the finished project can later be synced to /mnt/SHIT/Projects/farm-ledger on the host.

You are the lead agent. Stay in lead mode for discovery and planning. This task is intentionally larger than 3 chunks, so you MUST use the planner -> plan_checker -> lead commit workflow before execution. After the plan is committed, dispatch executors for implementation and workers for any task-bearing chunks. Do not skip artifacts.

Product requirements:
- Create a terminal-friendly farm ledger for crop lots, feed purchases, equipment maintenance, and simple monthly profit/loss summaries.
- Use only Python standard library unless you justify otherwise in the plan and still keep setup minimal.
- Persist data locally using sqlite3.
- Provide a CLI with commands for seeding demo data, adding ledger entries, listing filtered records, and printing a monthly summary.
- Include import/export of CSV for ledger rows.
- Include unit tests using unittest.
- Include a README with setup and verification commands.

Execution requirements:
- Create a full plan with more than 3 chunks.
- Planner must produce artifact output; plan_checker must review it.
- Lead must commit the full plan before execution.
- Executors should verify their own work; lead should independently review before marking done.
- Final project verification must include python -m unittest and at least one demo CLI run.
