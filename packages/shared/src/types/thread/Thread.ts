import type { IAgent } from "../agent/IAgent";
import type { HostConfig } from "../host/IHost";
import type { ICoordinator } from "../coordinator/ICoordinator";
import type { AgentTurn } from "../agent/AgentIO";
import type { AgentConversationalMessage } from "../agent/AgentHistoryData";

export interface IThread {
  readonly id: string;
  readonly hostConfig: HostConfig;
  readonly rootCwd: string;
  readonly leadAgent: IAgent;
  readonly coordinator: ICoordinator;
  title?: string;
  tokensUsed?: number;
  checkpointedAt?: number;
  interrupted?: boolean;
  getSubagents(): IAgent[];
  getAllAgentIds(): string[];
  interrupt(): void;
  cancelRequest(): void;
  checkpoint(path?: string): Promise<void>;
  dispose(): Promise<void>;
  recordTurn(agentId: string, turn: AgentTurn): Promise<number>;
  recordMessage(agentId: string, message: AgentConversationalMessage): Promise<number>;
  forgetEntry(sequence: number): Promise<void>;
  unforgetEntry(sequence: number): Promise<void>;
  editUserMessage(sequence: number, newContent: string): Promise<void>;
  getLastUserMessage(): Promise<{ sequence: number; content: string } | null>;
  getLastAgentTurn(agentId: string): Promise<number | null>;
  forgetEventsAfterSequence(targetSequence: number): Promise<void>;
  forgetLastTurn(): Promise<void>;
  getJournalPath(): string;
  getAgentJournalEntries?(agentId: string): Promise<unknown[]>;
}
