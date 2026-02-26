import type { AgentStatus } from "../types/agent/AgentState";
import type { ModelInfo } from "../types/provider/ModelInfo";
import type { SSEMessage } from "../sse/types";

export type { AgentStatus, ModelInfo, SSEMessage };

export interface CreateThreadRequest {
  hostConfig: Record<string, unknown>;
  rootCwd: string;
  purpose: string;
  objective: string;
  workType: "Conversational" | "Goal";
  generationOptions?: ThreadGenerationOptions;
}

export interface ThreadGenerationOptions {
  maxTokens?: number;
  temperature?: number;
  topP?: number;
  frequencyPenalty?: number;
  presencePenalty?: number;
  modelId?: string;
  providerId?: string;
  reasoningEffort?: string;
}

export interface Message {
  sequence: number;
  isUser: boolean;
  content: string | unknown[];
  timestamp: Date;
  tokens: number;
  agentId?: string;
  type: "monologue" | "response" | "provider_request" | "provider_error";
  isMonologue?: boolean;
  thinking?: string;
  isStreaming?: boolean;
  providerRequest?: Record<string, unknown>;
  providerError?: {
    error: string;
    modelId?: string;
    providerId?: string;
  };
  toolCalls?: Array<{
    name: string;
    callId: string;
    status: "preparing" | "running" | "done" | "error";
    result?: unknown;
    summary?: string;
    args?: unknown;
    durationMs?: number;
    streamingOutput?: string;
    exitCode?: number;
    error?: string;
    metadata?: Record<string, any>;
    spawnedAgentId?: string;
  }>;
  turnCount?: number;
}

export interface MessageRequest {
  message: string | unknown[];
}

export interface EditMessageRequest {
  sequence: number;
  newContent: string;
}

export interface EditMessageResponse {
  sequence: number;
  message: Message;
}

export interface MessageListResponse {
  messages: Message[];
  total: number;
}

export interface ThreadResponse {
  id: string;
  title: string;
  rootCwd: string;
  leadAgentId: string;
  checkpointedAt: Date;
  agentCount: number;
  tokensLimit?: number;
  tokensUsed?: number;
  modelId?: string;
  providerId?: string;
  reasoningEffort?: string;
  hostType?: string;
}

export interface Thread extends ThreadResponse {
  modelId?: string;
  providerId?: string;
  reasoningEffort?: string;
}

export interface BranchThreadRequest {
  sequence: number;
  newContent: string;
}

export interface ProviderInfo {
  id: string;
  name: string;
  type: string;
  requiresApiKey: boolean;
  models: ModelInfo[];
  baseUrl?: string;
}

export interface AgentResponse {
  id: string;
  readableName: string;
  purpose: string;
  objective: string;
  isLead: boolean;
  parentId?: string;
  status: AgentStatus;
  modelId?: string;
  threadId: string;
  tokensUsed?: number;
  turnCount?: number;
  subagentIds?: string[];
}

export interface Agent extends AgentResponse { }

export interface AgentHierarchy {
  agent: Agent;
  children: AgentHierarchy[];
}

export interface UserConfig {
  defaultModels: PurposeDefaultModel[];
}

export interface PurposeDefaultModel {
  purpose: string;
  modelId: string;
  providerId: string;
}

export interface ThreadEventsResponse {
  events: any[];
  agents: AgentResponse[];
}

export interface SSHConfig {
  host: string;
  port: number;
  user: string;
  auth: {
    type: "password" | "key";
    value: string;
  };
}

export interface APIError {
  error: string;
  details?: unknown;
}

export interface APIErrorResponse {
  error: string;
  statusCode?: number;
  details?: unknown;
}
