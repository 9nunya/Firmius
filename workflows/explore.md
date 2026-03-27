---
name: Parallel Exploration
description: Explore the current working directory with parallel subagents
---

I need you to explore the current working directory and create a comprehensive analysis. Use parallel subagents to speed up the exploration.

**Your Task:**

1. First, list the directory structure to understand what we're working with
2. Spawn 3 scouts with the following specializations:
   - **Scout 1 (Code Analysis)**: Analyze source code files, identify programming languages, frameworks, and architecture patterns
   - **Scout 2 (Documentation)**: Review README, docs, comments, and configuration files to understand the project purpose
   - **Scout 3 (Dependencies)**: Examine package.json, requirements.txt, CMakeLists.txt, or other dependency files

3. Each subagent should produce a focused report on their area

4. After all subagents complete, synthesize their findings into a unified summary that includes:
   - Project overview
   - Technology stack
   - Directory structure highlights
   - Key files and their purposes
   - Any potential issues or recommendations

Use `summon_subagent` with `async=true` to create each subagent with a clear persona and task. Wait for all to complete before synthesizing.
