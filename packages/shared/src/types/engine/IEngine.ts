import type { IProvider } from "../provider/IProvider";
import type { ITool } from "../tool/ITool";
import type { IHost } from "../host/IHost";
import type { AgentStatus } from "../agent/AgentState";
import type { IAgentFactory } from "../agent/IAgentFactory";
import type { IThread } from "../thread/Thread";
import type { ILSPUtility } from "../lsp/ILSP";

export interface IgniteOptions {
  prettyPrint?: boolean;
}

export interface IEngine {
  providers: Record<string, IProvider>;
  tools: Record<string, ITool<unknown, unknown>>;
  agentFactory: IAgentFactory;
  threads: Map<string, IThread>;
  ignite(options?: IgniteOptions): void;
  loadProviders(): void;
  loadTools(): void;
  getLSPUtility(host: IHost, rootUri: string): ILSPUtility;
  getThread(id: string): IThread | undefined;
  addThread(thread: IThread): void;
  removeThread(id: string): void;
}

export interface BaseEngineEvent {
  timestamp: number;
  threadId: string;
}

export interface AgentSpawnedEvent extends BaseEngineEvent {
  type: 'agent_spawned';
  agentId: string;
  readableName: string;
  purpose: string;
  parentId?: string;
  modelId?: string;
  isLead?: boolean;
  taskContext?: string;
}

export interface AgentThinkingEvent extends BaseEngineEvent {
  type: 'agent_thinking';
  agentId: string;
  readableName: string;
  content: string;
  turnCount: number;
}

export interface AgentContentEvent extends BaseEngineEvent {
  type: 'agent_content';
  agentId: string;
  readableName: string;
  content: string;
  isComplete: boolean;
  turnCount: number;
}

export interface AgentProviderRequestEvent extends BaseEngineEvent {
  type: 'agent_provider_request';
  agentId: string;
  readableName: string;
  request: Record<string, unknown>;
  turnCount: number;
}

export interface ToolCallStartEvent extends BaseEngineEvent {
  type: 'tool_call_start';
  agentId: string;
  readableName: string;
  toolName: string;
  callId: string;
  arguments: Record<string, unknown>;
  summary: string;
  turnCount: number;
}

export interface ToolCallEndEvent extends BaseEngineEvent {
  type: 'tool_call_end';
  agentId: string;
  readableName: string;
  toolName: string;
  callId: string;
  result: unknown;
  summary: string;
  durationMs: number;
  success: boolean;
  turnCount: number;
}

export interface ToolCallPreparingEvent extends BaseEngineEvent {
  type: 'tool_call_preparing';
  agentId: string;
  readableName?: string;
  toolName: string;
  callId: string;
  turnCount?: number;
}

export interface AgentTerminatedEvent extends BaseEngineEvent {
  type: 'agent_terminated';
  agentId: string;
  readableName: string;
  reason: string;
  success: boolean;
  turnCount?: number;
}

export interface AgentFileChangedEvent extends BaseEngineEvent {
  type: 'agent_file_changed';
  agentId: string;
  readableName: string;
  filePath: string;
}

export interface AgentModelChangedEvent extends BaseEngineEvent {
  type: 'agent_model_changed';
  agentId: string;
  readableName: string;
  newModelId?: string;
  newProviderId?: string;
}

export interface UserMessageEvent extends BaseEngineEvent {
  type: 'user_message';
  agentId: string;
  content: string;
  sequence?: number;
}

export interface AgentStatusEvent extends BaseEngineEvent {
  type: 'agent_status';
  agentId: string;
  status: AgentStatus;
}

export interface AgentMetricsEvent extends BaseEngineEvent {
  type: 'agent_metrics';
  agentId: string;
  tokensUsed: number;
  tokensLimit: number;
  contextUsage: number;
}

export interface AgentProviderErrorEvent extends BaseEngineEvent {
  type: 'agent_provider_error';
  agentId: string;
  readableName: string;
  error: string;
  modelId?: string;
  providerId?: string;
  turnCount: number;
}

export interface AgentTurnCompleteEvent extends BaseEngineEvent {
  type: 'agent_turn_complete';
  agentId: string;
  readableName: string;
  turnCount: number;
}

export interface ProcessOutputEvent extends BaseEngineEvent {
  type: 'process_output';
  processId: string;
  pid?: number;
  data: string;
  source: 'stdout' | 'stderr';
}

export interface ProcessExitEvent extends BaseEngineEvent {
  type: 'process_exit';
  processId: string;
  pid?: number;
  exitCode: number;
  durationMs: number;
}

export interface ToolCallUpdateEvent extends BaseEngineEvent {
  type: 'tool_call_update';
  agentId: string;
  callId: string;
  metadata?: Record<string, any>;
  summary?: string;
}

export interface EngineAgentState {
  status: AgentStatus;
  lastEventTimestamp: number;
}

export type EngineEvent =
  | AgentSpawnedEvent
  | AgentThinkingEvent
  | AgentContentEvent
  | AgentProviderRequestEvent
  | ToolCallStartEvent
  | ToolCallEndEvent
  | ToolCallPreparingEvent
  | AgentTerminatedEvent
  | AgentFileChangedEvent
  | AgentModelChangedEvent
  | UserMessageEvent
  | AgentStatusEvent
  | AgentMetricsEvent
  | ProcessOutputEvent
  | ProcessExitEvent
  | ToolCallUpdateEvent
  | AgentProviderErrorEvent
  | AgentTurnCompleteEvent;
