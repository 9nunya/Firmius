import { StateManager } from "./state/StateManager";
import {
  FleetRegistry,
  type AgentStatus,
  type AgentRecord,
} from "./state/FleetRegistry";

import { Engine } from "./Engine";
import type { IHost } from "@firmius/shared";

type SpawnOptions = {
  purpose: string;
  objective: string;
  agentId: string;
  allowPaths?: string[];
  injectedFiles?: { path: string; content: string }[];
  parentId: string;
  cwd?: string;
};

type FleetStatus = {
  agents: AgentRecord[];
  health: "healthy" | "degraded" | "critical";
  stuckCount: number;
  runningCount: number;
};

type HealthReport = {
  healthy: AgentRecord[];
  stuck: AgentRecord[];
  recommendations: string[];
};

export class Coordinator {
  private stateManager: StateManager;
  private fleetRegistry: FleetRegistry;

  public readonly host: IHost;
  public readonly baseDir: string;

  private constructor(
    stateManager: StateManager,
    host: IHost,
    baseDir: string,
  ) {
    this.stateManager = stateManager;
    this.host = host;
    this.baseDir = baseDir;
    this.fleetRegistry = new FleetRegistry(this.stateManager);
  }

  static async create(
    host: IHost,
    baseDir: string,
    threadId: string,
  ): Promise<Coordinator> {
    const stateManager = await StateManager.create(host, baseDir, threadId);
    return new Coordinator(stateManager, host, baseDir);
  }

  async destroy(): Promise<void> {
    await this.stateManager.close();
  }

  get fleet(): FleetRegistry {
    return this.fleetRegistry;
  }

  async spawnAgent(options: SpawnOptions): Promise<{ agentId: string }> {
    await this.fleetRegistry.registerAgent({
      id: options.agentId,
      purpose: options.purpose,
      parentId: options.parentId,
    });
    return { agentId: options.agentId };
  }

  async getAgentStatus(agentId: string): Promise<AgentStatus> {
    const agent = await this.fleetRegistry.getAgent(agentId);
    return agent?.status ?? "idle";
  }

  async getFleetStatus(): Promise<FleetStatus> {
    const agents = await this.fleetRegistry.getRunningAgents();
    const healthReport = await this.checkHealth();
    const healthStatus: "healthy" | "degraded" | "critical" =
      healthReport.stuck.length === 0
        ? "healthy"
        : healthReport.stuck.length < agents.length
          ? "degraded"
          : "critical";
    return {
      agents,
      health: healthStatus,
      stuckCount: healthReport.stuck.length,
      runningCount: agents.length,
    };
  }

  async checkHealth(): Promise<HealthReport> {
    const running = await this.fleetRegistry.getRunningAgents();
    const stuck = await this.fleetRegistry.getStuckAgents(
      2 * 60 * 1000,
      5 * 60 * 1000,
    );
    const recommendations = stuck.map(
      (a: AgentRecord) =>
        `Agent ${a.id} appears stuck. Last heartbeat: ${Math.round(
          (Date.now() - (a.lastHeartbeat || 0)) / 1000,
        )}s ago. Consider: nudge, kill+respawn, or manual.`,
    );
    return {
      healthy: running.filter((a: AgentRecord) => !stuck.includes(a)),
      stuck,
      recommendations,
    };
  }

  async nudgeAgent(agentId: string, _message: string): Promise<void> {
    const agent = await this.fleet.getAgent(agentId);
    if (!agent) return;

    const actualAgent = Engine.agentFactory.agents.get(agent.id);
    if (actualAgent && actualAgent.status !== "working") actualAgent.actUntilAgentEnds();
  }

  async killAgent(agentId: string): Promise<void> {
    await this.fleetRegistry.updateStatus(agentId, "completed");
  }
}
