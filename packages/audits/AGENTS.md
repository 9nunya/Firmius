# AUDITS PACKAGE

**Purpose**: Benchmarking, evaluation, and stress testing harnesses.

---

## STRUCTURE

```
packages/audits/
├── include/
│   ├── AuditRegistry.hpp       # Singleton registry
│   └── audits/                 # Individual audit implementations
│       ├── ProviderAudit.hpp
│       ├── ProviderStreamDebugAudit.hpp
│       ├── CodexProviderAudit.hpp
│       ├── CodexQuotaAudit.hpp
│       ├── QwenProviderAudit.hpp
│       ├── QwenQuotaAudit.hpp
│       ├── AntigravityProviderAudit.hpp
│       ├── AntigravityQuotaAudit.hpp
│       ├── OAuthWizardAudit.hpp
│       ├── SubagentStressAudit.hpp
│       ├── HarnessChaosAudit.hpp
│       ├── BenchmarksAudit.hpp
│       └── WorkflowsAudit.hpp
├── src/audits/                 # Implementations
└── src/main.cpp                # Entry point (firmius_audit)
```

---

## KEY COMPONENTS

### AuditRegistry
- **File**: `include/AuditRegistry.hpp`
- **Pattern**: Singleton
- **Purpose**: Register and run all audits

### IAudit Interface
- **File**: `packages/shared/include/IAudit.hpp`
- **Key Methods**:
  - `run()` → Execute audit
  - `getName()` → Audit identifier

### Audit Categories

| Category | Audits |
|----------|--------|
| **Provider Tests** | ProviderAudit, CodexProviderAudit, QwenProviderAudit, AntigravityProviderAudit |
| **Quota Tests** | CodexQuotaAudit, QwenQuotaAudit, AntigravityQuotaAudit |
| **Auth Tests** | OAuthWizardAudit |
| **Stress Tests** | SubagentStressAudit, HarnessChaosAudit |
| **Benchmark Tests** | BenchmarksAudit |
| **Workflow Tests** | WorkflowsAudit |

---

## RUNNING AUDITS

```bash
./build/packages/audits/firmius_audit
```


### Reasoning Trace Continuity (Gemini 3 / Antigravity)

This audit is intended to reproduce and then prevent a regression where Gemini 3 Flash / antigravity Gemini 3* models only emit reasoning/thinking deltas on the first turn, and then stop emitting them on subsequent turns (especially after tool results).

**Build** (once core is green):
```bash
cmake --build build --target firmius_audit
```

**Run**:
```bash
./build/packages/audits/firmius_audit --audit reasoning_trace_continuity
```

**Expected**:
Fails on broken builds: turn 2+ reports `0 thinking chunks`.
Passes after the provider fix: all turns report `>0 thinking chunks`.

---

## ANTI-PATTERNS

| Pattern | Why Forbidden | Alternative |
|---------|---------------|-------------|
| Hardcoded credentials | Security | Use environment/config |
| Unbounded test execution | Resource limits | Set timeouts |
| Skipping cleanup | Resource leaks | Always cleanup in destructor |
