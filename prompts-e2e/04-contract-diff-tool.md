Build a greenfield C++20 project in /work/contract-diff-tool. This workspace is inside the Firmius debugging container; the finished project can later be synced to /mnt/SHIT/Projects/contract-diff-tool on the host.

Operate as the Firmius lead and force the full planner pipeline because the work is larger than 3 chunks. Use planner and plan_checker artifacts, then commit the accepted plan before any implementation. Dispatch executors and workers after that.

Project requirements:
- Use CMake.
- Build a CLI tool that compares two plain-text contract files and reports added, removed, and changed clauses using stable section identifiers.
- Provide a small sample dataset and scripted demo.
- Add unit tests (CTest is fine).
- README must document build and test commands.

Verification requirements:
- cmake -S . -B build
- cmake --build build
- ctest --test-dir build --output-on-failure
- one demo run against sample contracts
