import type { IHost } from "../host/IHost";
import type { AgentStatus } from "../agent/AgentState";

export interface AgentRecord {
  id: string;
  purpose: string;
  parentId: string | null;
  worktreePath: string | null;
  branch: string | null;
  status: AgentStatus;
  spawnedAt: number | null;
  completedAt: number | null;
  lastHeartbeat: number | null;
  lastActionTimestamp: number | null;
  lastProgressUpdate: number | null;
  currentTaskId: string | null;
  errorMessage: string | null;
}

export interface SpawnOptions {
  purpose: string;
  objective: string;
  agentId: string;
  allowPaths?: string[];
  injectedFiles?: { path: string; content: string }[];
  parentId: string;
  cwd?: string;
}

export interface FleetStatus {
  agents: AgentRecord[];
  health: "healthy" | "degraded" | "critical";
  stuckCount: number;
  runningCount: number;
}

export interface HealthReport {
  healthy: AgentRecord[];
  stuck: AgentRecord[];
  recommendations: string[];
}

export interface IFleetRegistry {
  registerAgent(agent: {
    id: string;
    purpose: string;
    parentId?: string;
    worktreePath?: string;
    branch?: string;
  }): Promise<void>;
  updateHeartbeat(agentId: string): Promise<void>;
  updateHeartbeatOnAction(agentId: string): Promise<void>;
  updateHeartbeatOnTurn(agentId: string): Promise<void>;
  updateProgressTimestamp(agentId: string): Promise<void>;
  updateStatus(agentId: string, status: AgentStatus): Promise<void>;
  getAgent(agentId: string): Promise<AgentRecord | null>;
  getRunningAgents(): Promise<AgentRecord[]>;
  getStuckAgents(
    heartbeatThresholdMs: number,
    progressThresholdMs: number,
    gracePeriodMs?: number,
  ): Promise<AgentRecord[]>;
  getChildren(parentId: string): Promise<AgentRecord[]>;
  deleteAgent(agentId: string): Promise<void>;
}

export interface ICoordinator {
  readonly host: IHost;
  readonly baseDir: string;
  readonly fleet: IFleetRegistry;

  spawnAgent(options: SpawnOptions): Promise<{ agentId: string }>;
  getAgentStatus(agentId: string): Promise<AgentStatus>;
  getFleetStatus(): Promise<FleetStatus>;
  checkHealth(): Promise<HealthReport>;
  nudgeAgent(agentId: string, message: string): Promise<void>;
  killAgent(agentId: string): Promise<void>;
  destroy(): Promise<void>;
}