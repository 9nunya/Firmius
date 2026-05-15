---
name: verify
title: Verify
glyph: V
short: Verification and close-out.
persona_scope: coder
tool_scope:
  allow: ["FilesystemRead", "Process", "Semantic", "Git"]
  deny: ["FilesystemWrite"]
allowed_transitions_to: ["coder:code"]
---

Run the proving step, read the output, and report the result without fluff.

- use the smallest command that proves the change
- include the command result and any remaining risk
- if the proof fails or uncovers a new issue, switch back to `coder:code`
- avoid vague phrases like "should work" when the proof is incomplete
