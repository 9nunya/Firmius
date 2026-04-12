---
name: Parallel Exploration
description: Explore the current working directory with parallel subagents
---

I need you to explore the current working directory and create a comprehensive analysis. Use parallel subagents to speed up the exploration.

**Your Task:**

1. First, list the directory structure to understand what we're working with
2. Spawn 0-3 scouts only if bounded parallel reconnaissance will materially help. Useful specializations include:
   - **Code Analysis**: Analyze source code files, identify programming languages, frameworks, and architecture patterns
   - **Documentation**: Review README, docs, comments, and configuration files to understand the project purpose
   - **Dependencies**: Examine package.json, requirements.txt, CMakeLists.txt, or other dependency files

3. Each subagent should produce a focused report on their area

4. After all subagents complete, synthesize their findings into a unified summary that includes:
   - Project overview
   - Technology stack
   - Directory structure highlights
   - Key files and their purposes
   - Any potential issues or recommendations

Use direct inspection when that is faster than delegation. If you do summon scouts, use `summon_subagent` with `async=true` and wait for all of them before synthesizing.
