export type AgentStatus = "initializing" | "working" | "completed" | "idle";

export interface AgentState {
  status: AgentStatus;
  metrics: AgentMetrics;
  todos: TodoItem[];
  nextTodoId: number;
  ownedProcesses: string[];
  budget?: import("../budget/BudgetTypes").BudgetState;
  checkpoints?: import("../budget/BudgetTypes").CheckpointData[];
}

export interface AgentMetrics {
  totalTokens: number;
  lastTurnTokens: number;
  lastPromptTokens: number;
  startTime: number;
}

export interface TodoItem {
  id: number;
  content: string;
  status: "pending" | "in_progress" | "completed";
  priority: "high" | "medium" | "low";
  createdAt: number;
}
