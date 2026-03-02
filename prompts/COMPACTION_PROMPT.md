# CONTEXT COMPACTION PROTOCOL

You are performing a recursive context consolidation. The conversation history is reaching its limit, and you must now summarize the entire session into a single "Synthetic Memory" to free up space while preserving all critical state.

## REQUIRED SUMMARY STRUCTURE

### 1. MISSION STATUS
- Original Objective: [Clear restatement of the user's task]
- Current Progress: [Summary of what has been achieved so far]
- Final Verification: [How we will know the task is complete]

### 2. CORE FINDINGS
- Discovered Facts: [Key technical details, file paths, and environment constraints identified]
- Obstacles Encountered: [Errors, dead ends, or failed attempts and how they were resolved]

### 3. WORLD STATE (CRITICAL)
- Modified Files: [List of all files changed and the nature of the changes]
- Active Processes: [Any background processes or tools currently in flight]
- Variables/State: [Any internal state or variables that must be tracked]

### 4. PENDING ACTIONS
- Immediate Next Step: [The very next thing the agent should do]
- Remaining Backlog: [List of remaining sub-tasks in priority order]

## CONSOLIDATION RULE
If a previous "COMPACTION SUMMARY" exists in the history, you MUST merge its contents into this new summary. Do not lose information from previous compactions.

## OUTPUT REQUIREMENTS
- Be concise but technically precise.
- Use Markdown formatting.
- Ensure the agent can resume execution immediately based ONLY on this summary and the original task.
- DO NOT use conversational filler. Provide the structured state only.
