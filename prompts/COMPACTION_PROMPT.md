# CONTEXT COMPACTION PROTOCOL

You are performing a recursive context consolidation. The conversation history is reaching its limit, and you must now summarize the entire session into a single "Synthetic Memory" to free up space while preserving all critical state.

## FACTUAL STATE INJECTION (READ CAREFULLY)

The following section contains **ground truth state** about the session. You MUST incorporate this information into your summary. Do NOT guess or assume - use the actual data provided below.

(Actual factual state will be provided above this prompt when compaction occurs.)

Your summary must accurately reflect this factual state. For example, if the state shows "Files Edited: /work/django/core/cache/backends/db.py", then your "WORLD STATE" section must reflect this. If "Modified Files: None yet (fix pending)" appears in your summary but files WERE edited, you are NOT following this instruction correctly.

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
