import type { AgentStatus } from "../agent/AgentState";

export interface JournalEntry {
  id: string;
  threadId: string;
  agentId: string;
  timestamp: number;
  status: AgentStatus;
  message?: string;
  metadata?: Record<string, unknown>;
}

export interface Journal {
  entries: JournalEntry[];
  addEntry(entry: Omit<JournalEntry, "id" | "timestamp">): void;
  getEntriesForAgent(agentId: string): JournalEntry[];
  getEntriesForThread(threadId: string): JournalEntry[];
}
