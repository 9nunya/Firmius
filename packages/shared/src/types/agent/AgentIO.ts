import type { ProviderToolCall } from "../provider/IProvider";
import type { ProviderMessageContentPart } from "../provider/IProvider";

export interface AgentTurn {
  toolCalls: ProviderToolCall[];
  toolResults: AgentToolResult[];
  reasoning: string;
  content?: string | ProviderMessageContentPart[];
  timestamp: number;
  tokens: number;
  summary?: string;
  protected?: boolean;
  interrupted?: boolean;
  completed?: boolean;
}

export interface AgentToolResult {
  id: string;
  result: unknown;
}

export interface AgentIO {
  watchFile?: (path: string) => void;
  unwatchFile?: (path: string) => void;
  onTurn?: (turn: AgentTurn, agentId: string) => Promise<number | void>;
  onCheckpoint?: (agentId: string) => Promise<void>;
}
