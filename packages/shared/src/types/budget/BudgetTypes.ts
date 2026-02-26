export interface BudgetConfig {
  fileBudgetPercent: number;
  protectedBudgetPercent: number;
  rollingBudgetPercent: number;
  reserveBudgetPercent: number;
  systemBudgetPercent: number;
  maxRollingTurns: number;
}

export const DEFAULT_BUDGET_CONFIG: BudgetConfig = {
  fileBudgetPercent: 0.25,
  protectedBudgetPercent: 0.15,
  rollingBudgetPercent: 0.4,
  reserveBudgetPercent: 0.1,
  systemBudgetPercent: 0.1,
  maxRollingTurns: 21,
};

export type EntryPriority = "critical" | "high" | "normal" | "low";

export interface BufferEntry {
  turnIndex: number;
  toolCallId: string;
  toolName: string;
  content: string;
  tokenCount: number;
  timestamp: number;
  priority: EntryPriority;
}

export interface FileWatchEntry {
  path: string;
  offset: number;
  limit: number;
  content: string;
  charCount: number;
  mtime: number;
  isFullFile: boolean;
}

export type ProtectedEntryType =
  | "objective"
  | "anchor"
  | "user_message"
  | "key_fact";

export interface ProtectedEntry {
  type: ProtectedEntryType;
  content: string;
  timestamp: number;
  metadata?: Record<string, unknown>;
}

export interface CheckpointData {
  sequence: number;
  threadId: string;
  agentId: string;
  timestamp: number;
  summary: string;
  preservedFacts: Record<string, string>;
  preservedDecisions: string[];
  originalTokens: number;
  summaryTokens: number;
}

export interface CompactionJob {
  id: string;
  entries: BufferEntry[];
  status: "pending" | "running" | "complete" | "failed";
  result?: CheckpointData;
  error?: string;
}

export interface BudgetState {
  modelCtx: number;
  lastPromptTokens: number;
  lastCompletionTokens: number;
  systemUsage: number;
  protectedUsage: number;
  rollingUsage: number;
  fileUsageChars: number;
}

export interface BudgetAllocation {
  fileBudget: number;
  protectedBudget: number;
  rollingBudget: number;
  reserveBudget: number;
  systemBudget: number;
}

export interface EvictionCandidate {
  type: "tool_result" | "turn" | "reasoning";
  turnIndex: number;
  tokenCount: number;
  priority: EntryPriority;
}

export interface IBudgetComponents {
  tracker: any;
  rollingBuffer: any;
  fileWatchBudget: any;
  protectedContext: any;
  checkpoints: CheckpointData[];
}