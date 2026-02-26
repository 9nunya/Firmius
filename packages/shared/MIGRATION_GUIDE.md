# AgentStatus Migration Guide

**Version:** 1.0  
**Date:** 2026-02-21  
**Scope:** AgentStatus type consolidation across the codebase

---

## Overview

This guide documents the migration from fragmented `AgentStatus` type definitions to a unified `AgentStatus` type in the shared package.

**Important:** This is a prototype migration. No database schema changes are required at this time.

---

## AgentStatus Values

The unified `AgentStatus` type consolidates all status values from multiple sources:

```typescript
export type AgentStatus =
  | "initializing"
  | "spawning"
  | "idle"
  | "thinking"
  | "working"
  | "waiting"
  | "running"
  | "completed"
  | "terminated"
  | "failed"
  | "stuck"
  | "error";
```

---

## Status Value Mappings

### Legacy to Standardized Mappings

| Legacy Status | Standardized Status | Source |
|---------------|---------------------|--------|
| `spawning` | `spawning` | Direct mapping |
| `running` | `running` | Direct mapping |
| `completed` | `completed` | Direct mapping |
| `failed` | `failed` | Direct mapping |
| `stuck` | `stuck` | Direct mapping |
| `error` | `failed` | **Migrated** |
| `initializing` | `initializing` | Direct mapping |
| `idle` | `idle` | Direct mapping |
| `thinking` | `thinking` | Direct mapping |
| `working` | `working` | Direct mapping |
| `waiting` | `waiting` | Direct mapping |
| `terminated` | `terminated` | Direct mapping |
| `active` | `idle` | **Migrated** |

### Important Changes

1. **`error` → `failed`**: The legacy `error` status is now mapped to `failed` for consistency with lifecycle terminology.

2. **`active` → `idle`**: The legacy `active` status (from `AgentRuntimeStatus`) is now mapped to `idle` to unify runtime status with interactive status.

---

## Normalization Helper

Use the `normalizeAgentStatus()` function to convert legacy status values to the standardized format:

```typescript
import { normalizeAgentStatus } from '@firmius/shared';

const legacyStatus = 'error';
const standardizedStatus = normalizeAgentStatus(legacyStatus);
console.log(standardizedStatus); // Output: 'failed'
```

### Type Guard

Use `isLegacyStatus()` to check if a value is a valid legacy status:

```typescript
import { isLegacyStatus, normalizeAgentStatus } from '@firmius/shared';

const status = 'spawning';
if (isLegacyStatus(status)) {
  const normalized = normalizeAgentStatus(status);
}
```

---

## Import Path Changes

### Before

```typescript
// Various locations had different AgentStatus definitions
import type { AgentStatus } from '../../core/state/types';
import type { AgentStatus } from '../api/types/api';
// etc.
```

### After

```typescript
// Import from shared package
import type { AgentStatus } from '@firmius/shared';
import { normalizeAgentStatus } from '@firmius/shared';
```

---

## Migration Steps

### 1. Update Imports

Replace all `AgentStatus` type imports with the shared version:

```typescript
// Before
import type { AgentStatus } from './types';

// After
import type { AgentStatus } from '@firmius/shared';
```

### 2. Normalize Status Values

When reading status values from legacy sources, normalize them:

```typescript
// Before
const status = agent.status; // Could be 'error' or 'failed'

// After
const status = normalizeAgentStatus(agent.status); // Always standardized
```

### 3. Remove Duplicate Definitions

Remove local `AgentStatus` type definitions and use the shared version.

---

## Affected Files

The following files need to be updated to use the shared `AgentStatus`:

- `src/core/state/types.ts` - Lifecycle statuses
- `src/core/state/FleetRegistry.ts` - Database statuses
- `src/api/types/api.ts` - Frontend statuses
- `src/web/src/components/fleet/FleetSidebar.tsx` - UI component
- `src/tui/components/modules/FleetModal.tsx` - TUI component
- `src/types/IEngine.ts` - Engine event statuses

---

## Database Schema (Future)

**Note:** No database migration is required for this prototype.

When the database schema is finalized, the following changes will be needed:

1. Update the `agents` table to use the comprehensive `AgentStatus` enum
2. Migrate existing records with `error` status to `failed`
3. Migrate existing records with `active` status to `idle`
4. Update all database queries to use the new status values

Example migration SQL (for future reference):

```sql
-- Normalize agent status values
UPDATE agents SET status = 'failed' WHERE status = 'error';
UPDATE agents SET status = 'idle' WHERE status = 'active';

-- Ensure status column uses the new enum type
ALTER TABLE agents 
MODIFY COLUMN status ENUM(
  'initializing', 'spawning', 'idle', 'thinking', 'working',
  'waiting', 'running', 'completed', 'terminated', 'failed', 'stuck', 'error'
);
```

---

## Testing Checklist

- [ ] All imports resolve to `@firmius/shared` AgentStatus
- [ ] `normalizeAgentStatus()` correctly maps all legacy values
- [ ] No duplicate `AgentStatus` definitions in the codebase
- [ ] All status comparisons use the standardized values
- [ ] UI components display correct status labels
- [ ] Database queries (when implemented) use standardized values

---

## References

- **Analysis Document:** `TYPE_CONFLICT_ANALYSIS.md` (Section M-1)
- **Type Definition:** `packages/shared/src/types/agent/AgentState.ts`
- **Normalization Helper:** `packages/shared/src/utils/agent-status.ts`
- **Related Tasks:** B-060, B-061, C-050, C-051

---

## Support

If you encounter issues during migration:

1. Check that all imports are from `@firmius/shared`
2. Verify that status values are being normalized when reading from legacy sources
3. Ensure no local `AgentStatus` type definitions remain
4. Run `bun run typecheck` to catch type mismatches

---

**End of Migration Guide**
