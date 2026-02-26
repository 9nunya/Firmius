import type {
  ThreadResponse,
  AgentResponse,
  AgentStatus,
  Message,
  APIError as BackendAPIError,
} from "@firmius/shared/api";

import { AgentWorkType } from "@firmius/shared/types";

export type Thread = ThreadResponse & {
  modelId?: string;
  providerId?: string;
  reasoningEffort?: string;
};

export type Agent = AgentResponse;

// Remove duplicate - Message is imported at top

export type APIError = BackendAPIError;

export type Event =
  | ThreadCreatedEvent
  | MessageAddedEvent
  | AgentSpawnedEvent
  | AgentStatusChangedEvent
  | AgentThinkingEvent
  | AgentContentEvent
  | AgentProviderRequestEvent
  | AgentProviderErrorEvent
  | ToolCallStartEvent
  | ToolCallEndEvent
  | AgentTerminatedEvent
  | AgentMetricsEvent
  | ProcessOutputEvent
  | ProcessExitEvent
  | ToolCallUpdateEvent
  | ErrorEvent;

export interface ToolCallUpdateEvent {
  type: "tool_call_update";
  threadId: string;
  agentId: string;
  timestamp: Date;
  callId: string;
  metadata?: Record<string, any>;
  summary?: string;
}

export interface ThreadCreatedEvent {
  type: "thread_created";
  threadId: string;
  timestamp: Date;
}

export interface MessageAddedEvent {
  type: "message_added";
  threadId: string;
  message: Message;
  timestamp: Date;
}

export interface AgentSpawnedEvent {
  type: "agent_spawned";
  threadId: string;
  agentId: string;
  readableName: string;
  purpose: string;
  isLead: boolean;
  parentId?: string;
  modelId?: string;
  timestamp: Date;
}

export interface AgentStatusChangedEvent {
  type: "agent_status_changed";
  threadId: string;
  agentId: string;
  status: AgentStatus;
  timestamp: Date;
  turn?: number;
}

export interface AgentThinkingEvent {
  type: "agent_thinking";
  threadId: string;
  agentId: string;
  thought: string;
  turn: number;
  timestamp: Date;
}

export interface AgentContentEvent {
  type: "agent_content";
  threadId: string;
  agentId: string;
  content: string;
  isComplete: boolean;
  turn: number;
  timestamp: Date;
}

export interface AgentProviderRequestEvent {
  type: "agent_provider_request";
  threadId: string;
  agentId: string;
  request: Record<string, unknown>;
  turn: number;
  timestamp: Date;
}

export interface AgentProviderErrorEvent {
  type: "agent_provider_error";
  threadId: string;
  agentId: string;
  readableName?: string;
  error: string;
  modelId?: string;
  providerId?: string;
  turn: number;
  timestamp: Date;
}

export interface ToolCallStartEvent {
  type: "tool_call_start";
  threadId: string;
  agentId: string;
  toolName: string;
  callId?: string;
  arguments?: Record<string, unknown>;
  summary?: string;
  turn: number;
  timestamp: Date;
}

export interface ToolCallEndEvent {
  type: "tool_call_end";
  threadId: string;
  agentId: string;
  toolName: string;
  callId?: string;
  result?: unknown;
  summary?: string;
  error?: string;
  metadata?: Record<string, any>;
  executionTimeMs?: number;
  success: boolean;
  turn: number;
  timestamp: Date;
}

export interface AgentTerminatedEvent {
  type: "agent_terminated";
  threadId: string;
  agentId: string;
  reason: string;
  finalStatus: string;
  timestamp: Date;
}

export interface AgentMetricsEvent {
  type: "agent_metrics";
  threadId: string;
  agentId: string;
  tokensUsed: number;
  apiCalls: number;
  toolCalls: number;
  executionTimeMs: number;
  timestamp: Date;
}

export interface ProcessOutputEvent {
  type: "process_output";
  threadId: string;
  agentId: string;
  processId: string;
  pid?: number;
  data: string;
  source: 'stdout' | 'stderr';
  timestamp: Date;
}

export interface ProcessExitEvent {
  type: "process_exit";
  threadId: string;
  agentId: string;
  processId: string;
  pid?: number;
  exitCode: number;
  durationMs: number;
  timestamp: Date;
}

export interface ErrorEvent {
  type: "error";
  threadId?: string;
  agentId?: string;
  error: APIError;
  timestamp: Date;
}

export interface AppState {
  threads: Thread[];
  currentThreadId: string | null;
  agents: Record<string, Agent>;
  messages: Record<string, Message[]>;
  isLoading: boolean;
  error: APIError | null;
  events: Event[];
  selectedAgentId: string | null;
}

export type {
  AgentStatus,
  ThreadResponse,
  AgentResponse,
  ProviderInfo,
  ModelInfo,
  CreateThreadRequest,
  Message,
} from "@firmius/shared/api";

export { AgentWorkType };

export type { UserConfig } from "./UserConfig";