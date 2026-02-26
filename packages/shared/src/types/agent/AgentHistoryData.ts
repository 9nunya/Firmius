import type { AgentTurn } from "./AgentIO";
import type { ProviderMessageContentPart } from "../provider/IProvider";
export type { ProviderMessageContentPart };

export interface AgentConversationalMessage {
  isUser: boolean;
  content: string | ProviderMessageContentPart[];
  reasoning?: string;
  timestamp: number;
  tokens: number;
  summary?: string;
  protected?: boolean;
}

export function isAgentConversationalMessage(
  supposed: unknown,
): supposed is AgentConversationalMessage {
  return (supposed as AgentConversationalMessage).content !== undefined;
}

export interface AgentWorkflow {
  turns: AgentTurn[];
  timestamp: number;
  completed: boolean;
  finalMessage: string | null;
}

export function isAgentWorkflow(supposed: unknown): supposed is AgentWorkflow {
  return (supposed as AgentWorkflow).turns !== undefined;
}

export interface AgentConversation {
  history: Array<AgentConversationalMessage | AgentWorkflow>;
}

export enum AgentWorkType {
  Conversational,
  Goal,
}

export interface AgentHistory {
  type: AgentWorkType;
  workflow?: AgentWorkflow;
  conversation?: AgentConversation;
}

export interface AgentHistoryData {
  history: AgentHistory;
  reasoningHistory: Array<{ timestamp: number; reasoning: string }>;
  reasoningHistoryLimit: number;
}
