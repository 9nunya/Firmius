import type { EngineEvent } from "../types/engine/IEngine";
import type { AgentStatus } from "../types/agent/AgentState";

/**
 * Server-Sent Events (SSE) - Direct alignment with Engine Events.
 * This eliminates the manual conversion overhead and data loss.
 */
export type SSEMessage = EngineEvent;
export type SSEType = EngineEvent['type'];

export type SSEStatus = "connected" | "disconnected" | "connecting" | "error";

// Legacy data structures kept for schema compatibility during migration
export type SSEData = any;

export interface ToolCallUpdatedData {
  callId: string;
  metadata?: Record<string, any>;
  summary?: string;
}

export interface AgentStatusChangedData {
  status: AgentStatus;
  reason?: string;
  turn?: number;
}

export interface ToolCallPreparingData {
  toolName: string;
  callId: string;
  turn?: number;
}

export interface ToolCallStartData {
  toolName: string;
  arguments?: Record<string, unknown>;
  callId?: string;
  summary?: string;
  turn?: number;
}

export interface ToolCallEndData {
  toolName: string;
  callId?: string;
  result?: unknown;
  summary?: string;
  executionTimeMs?: number;
  success?: boolean;
  status?: "done" | "error";
  error?: string;
  metadata?: Record<string, any>;
  turn?: number;
}

export interface AgentSpawnedData {
  agentId: string;
  readableName: string;
  purpose: string;
  isLead?: boolean;
  parentId?: string;
  modelId?: string;
  taskContext?: string;
}

export interface AgentTerminatedData {
  reason: string;
  success: boolean;
  finalStatus?: string;
  turn?: number;
}

export interface UserMessageData {
  content: string;
  sequence?: number;
  isUser?: boolean;
  timestamp?: number;
  tokens?: number;
  type?: string;
  addedType?: string;
}

export interface AgentMetricsData {
  tokensUsed: number;
  tokensLimit: number;
  contextUsage: number;
  apiCalls?: number;
  toolCalls?: number;
  executionTimeMs?: number;
}

export interface AgentThinkingData {
  thought: string;
  turn?: number;
}

export interface AgentContentData {
  content: string;
  isComplete?: boolean;
  turn?: number;
}

export interface AgentProviderRequestData {
  request: Record<string, unknown>;
  turn?: number;
}

export interface AgentProviderErrorData {
  error: string;
  modelId?: string;
  providerId?: string;
  turn?: number;
}

export interface AgentStatusData {
  status: AgentStatus;
}

export interface AgentFileChangedData {
  filePath: string;
}

export interface ThreadCreatedData {
  purpose?: string;
  modelId?: string;
}

export interface MessageAddedData {
  message: Record<string, unknown>;
}

export interface ErrorOccurredData {
  error: {
    message: string;
    code?: string;
  };
  context?: string;
}

export interface ProcessOutputData {
  processId: string;
  pid?: number;
  data: string;
  source: "stdout" | "stderr";
}

export interface ProcessExitData {
  processId: string;
  pid?: number;
  exitCode: number;
  durationMs: number;
}

export interface EventHistoryResponse {
  events: SSEMessage[];
  agents: any[];
  timestamp: string | number | Date;
}

export type SSEAgentInfo = any;

export interface SSEClientConfig {
  baseUrl?: string;
  reconnect?: boolean;
  reconnectInitialDelay?: number;
  reconnectMaxDelay?: number;
  eventBatchWindow?: number;
}

export interface SSEConnectionOptions {
  threadId: string;
  baseUrl?: string;
}
