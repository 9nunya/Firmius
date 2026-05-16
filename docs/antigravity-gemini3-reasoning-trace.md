# Gemini 3 / Antigravity reasoning trace regression

## Symptom

On Gemini 3 Flash and other antigravity Gemini 3* models, the provider emits reasoning/thinking deltas only on the **first** user turn (or a turn after the agent was aborted / history reset). After that, the model stops emitting separate reasoning/thinking deltas and instead produces the answer directly as normal content.

This is most visible on turn 2+ and after tool-result turns where the agent should decide next actions.

## Root cause

Firmius persisted hidden reasoning as `ThinkingContent` in history and **replayed it back** to Antigravity/Gemini by converting it into normal `text` parts inside the next request.

Empirically, Gemini 3 / antigravity reacts to seeing prior hidden reasoning in the prompt by suppressing separate thinking output on subsequent turns.

## Fix

Stop replaying historical `ThinkingContent` back to Antigravity/Gemini. The model will still emit fresh thinking chunks for the current turn.

Patch location:
- `packages/provider/src/providers/AntigravityProtocol.cpp`
  - In `prepareRequestBody(...)`, skip `ThinkingContent` parts.

## Reproduction / verification (via firmius_audit)

### Build

```bash
cmake --build build --target firmius_audit
```

### Run

```bash
./build/packages/audits/firmius_audit --audit reasoning_trace_continuity
```

### Expected

- Broken behavior: Turn 2+ shows `0 thinking chunks`.
- Fixed behavior: All turns show `>0 thinking chunks`.

## Current blocker

At the time of writing, `firmius_audit` may fail to build due to **unrelated** warnings-as-errors in core tools.

Once that core build is green again, the audit above can be used to confirm the regression stays fixed.
