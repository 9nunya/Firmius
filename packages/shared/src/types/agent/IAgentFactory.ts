import type { IAgent, AgentContext } from "./IAgent";
import type { AgentPurpose } from "./AgentIdentity";
import type { AgentWorkType } from "./AgentHistoryData";
import type { AgentTurn } from "./AgentIO";

export interface AgentFactorySummonOptions {
  purpose: AgentPurpose;
  objective: string;
  readableName?: string;
  cwd?: string;
  host: unknown;
  workType?: AgentWorkType;
  threadId?: string;
  generationOptions?: Partial<AgentContext["execution"]["generationOptions"]>;
  parentId?: string;
  constraints?: {
    allowOutsideCwd?: boolean;
  };
  additionalContext?: {
    injectedContext?: string;
  };
  tags?: Record<string, string>;
  disableCompaction?: boolean;
  onTurn?: (turn: AgentTurn, agentId: string) => Promise<number | void>;
  onCheckpoint?: (agentId: string) => Promise<void>;
  injectedFiles?: Array<{ path: string; content: string }>;
  allowPaths?: string[];
}

export interface IAgentFactory {
  agents: Map<string, IAgent>;
  summon(options: AgentFactorySummonOptions): Promise<IAgent>;
  terminate(id: string): Promise<void>;
}
