import type { ModelInfo } from "../provider/ModelInfo";
import type { AgentIdentity } from "./AgentIdentity";
import type { AgentHistoryData } from "./AgentHistoryData";
import type { AgentEnvironment } from "./AgentEnvironment";
import type { AgentState, AgentStatus } from "./AgentState";
import type { AgentExecution } from "./AgentExecution";
import type { AgentIO, AgentTurn } from "./AgentIO";
import type { AgentConversationalMessage } from "./AgentHistoryData";

export interface AgentContext {
  identity: AgentIdentity;
  historyData: AgentHistoryData;
  environment: AgentEnvironment;
  state: AgentState;
  execution: AgentExecution;
  io: AgentIO;
}

export enum AgentActType {
  Response,
  Turn,
}

export interface AgentActResult {
  type: AgentActType;
  response?: AgentConversationalMessage;
  turn?: AgentTurn;
}

export interface IAgent {
  id: string;
  identity: AgentIdentity;
  environment: AgentEnvironment;
  state: AgentState;
  io: AgentIO;
  execution: AgentExecution;
  historyData: AgentHistoryData;
  context?: AgentContext;
  status: AgentStatus;
  act(): Promise<AgentActResult>;
  actUntilAgentEnds(): Promise<AgentActResult[]>;
  readableName: string;
  getTurnCount(): number;
  interrupt(): Promise<void>;
  forgetLastTurn(): Promise<void>;
  updateGenerationOptions(options: {
    modelId?: string;
    providerId?: string;
    reasoningEffort?: string;
    maxTokens?: number;
  }): void;
  getModelInfo(): ModelInfo | undefined;
}
