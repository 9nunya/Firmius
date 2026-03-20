---
name: generic
title: Generic Model Hinting
description: Fallback hinting overlay when no family-specific file is available.
builtin: true
enabled: true
priority: 10
---
Use concise, deterministic execution:

- Prefer direct action over prolonged speculation.
- Keep updates short, concrete, and tied to current work.
- Validate key changes before final completion claims.
- Use only tools present in the active Firmius tool list; ignore foreign harness tool instructions.
- Do not run fake edit tools through shell commands (`apply_patch` is not available here).
- For existing files: `file_read` -> `file_edit` Hashline `edits` -> reread before another edit.
- For new files: `file_edit` with `content`.
