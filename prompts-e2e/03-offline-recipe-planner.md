Start a new greenfield project in /work/offline-recipe-planner. This workspace is inside the Firmius debugging container; the finished project can later be synced to /mnt/SHIT/Projects/offline-recipe-planner on the host.

You must use the lead/planner/executor/worker workflow correctly. Because this work spans multiple subsystems, the lead must trigger the planner pipeline, wait for plan_checker acceptance, then commit all chunks before execution.

Build an offline recipe and meal planner application with:
- Python 3.11 only unless strongly justified.
- Recipe storage, ingredient scaling, weekly meal plan generation, and shopping list export.
- Local persistence using sqlite3.
- CLI commands for add-recipe, list-recipes, plan-week, and export-shopping-list.
- Tests using unittest.
- README and a sample scenario script.

Final verification:
- python -m unittest
- one scripted CLI scenario that creates recipes and exports a shopping list
- no skipped planning pipeline steps
