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
---

# Team Collaboration Session

**Task:** $2

**Team Size:** $1 subagents

## Instructions

You are the `lead` for a multi-agent collaboration session. Your job is to:

1. **Analyze the task** and break it down into subtasks that can be parallelized
2. **Spawn $1 specialized subagents** with clear personas and responsibilities
3. **Coordinate their work** by facilitating communication between them
4. **Synthesize their outputs** into a cohesive final result

## Suggested Subagent Roles

Based on the task, consider spawning subagents with these specializations:
- **Scout**: Gathers information, reads documentation, explores codebases
- **Executor**: Owns one chunk and performs implementation
- **Auditor**: Reviews risks, regressions, and missing verification
- **Worker**: Handles a tightly bounded executor-delegated subtask

## Workflow

1. First, explore the current directory to understand the context
2. Create a brief plan outlining which subagents you'll spawn and their responsibilities
3. Spawn each subagent using `summon_subagent` with one of the shipped personas and a bounded task
4. Wait for all subagents to complete their work
5. Review each subagent's output and provide feedback if needed
6. Synthesize all contributions into a final deliverable
7. Summarize what was accomplished and any follow-up recommendations

## Communication Guidelines

- Give each subagent a **specific, focused task** with boundaries and expected evidence
- Set **clear expectations** for deliverables and format
- Check in periodically on long-running subtasks
- Resolve conflicts if subagents produce contradictory work
- Ensure all subagents understand they should signal when their task is complete

Begin by analyzing the task and creating your subagent plan.
