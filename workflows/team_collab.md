---
name: Team Collaboration
description: Spawn multiple specialized subagents to collaborate on a complex task
args:
  - name: team_size
    type: number
    description: Number of subagents to spawn (2-5 recommended)
    optional: false
  - name: task
    type: string
    description: The main task or problem for the team to solve
    optional: false
  - name: working_dir
    type: filepath
    description: Directory to work in (defaults to current directory)
    optional: true
  - name: coordinator_role
    type: string
    description: Special role for the coordinator subagent (e.g., "architect", "tech lead")
    optional: true
---

# Team Collaboration Session

**Task:** $2

**Team Size:** $1 subagents

$4

## Instructions

You are the coordinator of a multi-agent collaboration session. Your job is to:

1. **Analyze the task** and break it down into subtasks that can be parallelized
2. **Spawn $1 specialized subagents** with clear personas and responsibilities
3. **Coordinate their work** by facilitating communication between them
4. **Synthesize their outputs** into a cohesive final result

## Suggested Subagent Roles

Based on the task, consider spawning subagents with these specializations:
- **Researcher**: Gathers information, reads documentation, explores codebases
- **Implementer**: Writes code, creates files, makes edits
- **Reviewer**: Reviews code quality, checks for bugs, validates solutions
- **Tester**: Creates tests, runs validation, ensures correctness
- **Documenter**: Writes documentation, adds comments, creates summaries

## Workflow

1. First, explore `$3` (or current directory if not specified) to understand the context
2. Create a brief plan outlining which subagents you'll spawn and their responsibilities
3. Spawn each subagent using `summon_subagent` with a clear persona description
4. Wait for all subagents to complete their work
5. Review each subagent's output and provide feedback if needed
6. Synthesize all contributions into a final deliverable
7. Summarize what was accomplished and any follow-up recommendations

## Communication Guidelines

- Give each subagent a **specific, focused task** - avoid vague instructions
- Set **clear expectations** for deliverables and format
- Check in periodically on long-running subtasks
- Resolve conflicts if subagents produce contradictory work
- Ensure all subagents understand they should signal when their task is complete

Begin by analyzing the task and creating your subagent plan.
