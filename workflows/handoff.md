---
name: Handoff
description: Force the focused agent to emit a full handoff bundle, then reset and re-seed it.
slash_command: /handoff
action:
  kind: script
  language: luau
  timeout_s: 180
  script_file: handoff.luau
returns:
  schema: HandoffBundle
---

This workflow captures current thread state, forces an immediate structured handoff, then resets and re-seeds the focused agent.
