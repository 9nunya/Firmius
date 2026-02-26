import { StateManager } from "./StateManager";
import type { AgentRecord, AgentStatus } from "@firmius/shared";

export type { AgentRecord, AgentStatus };

export class FleetRegistry {
  constructor(private state: StateManager) {}

  private mapRow(row: any): AgentRecord {
    return {
      id: row.id,
      purpose: row.purpose,
      parentId: row.parent_id,
      worktreePath: row.worktree_path,
      branch: row.branch,
      status: row.status as AgentStatus,
      spawnedAt: row.spawned_at,
      completedAt: row.completed_at,
      lastHeartbeat: row.last_heartbeat,
      lastActionTimestamp: row.last_action_timestamp,
      lastProgressUpdate: row.last_progress_update,
      currentTaskId: row.current_task_id,
      errorMessage: row.error_message,
    };
  }

  async registerAgent(agent: {
    id: string;
    purpose: string;
    parentId?: string;
    worktreePath?: string;
    branch?: string;
  }): Promise<void> {
    const db = this.state.getDB();
    const now = Date.now();
    db.query(
      `INSERT INTO agents (id, purpose, parent_id, worktree_path, branch, status, spawned_at, last_heartbeat, last_action_timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
    ).run(
      agent.id,
      agent.purpose,
      agent.parentId ?? null,
      agent.worktreePath ?? null,
      agent.branch ?? null,
      "spawning",
      now,
      now,
      now,
    );
  }

  async updateHeartbeat(agentId: string): Promise<void> {
    const db = this.state.getDB();
    db.query(`UPDATE agents SET last_heartbeat = ? WHERE id = ?`).run(
      Date.now(),
      agentId,
    );
  }

  async updateHeartbeatOnAction(agentId: string): Promise<void> {
    const db = this.state.getDB();
    const now = Date.now();
    db.query(
      `UPDATE agents SET last_heartbeat = ?, last_action_timestamp = ? WHERE id = ?`,
    ).run(now, now, agentId);
  }

  async updateHeartbeatOnTurn(agentId: string): Promise<void> {
    const db = this.state.getDB();
    const now = Date.now();
    db.query(
      `UPDATE agents SET last_heartbeat = ?, last_action_timestamp = ? WHERE id = ?`,
    ).run(now, now, agentId);
  }

  async updateProgressTimestamp(agentId: string): Promise<void> {
    const db = this.state.getDB();
    db.query(`UPDATE agents SET last_progress_update = ? WHERE id = ?`).run(
      Date.now(),
      agentId,
    );
  }

  async updateStatus(agentId: string, status: AgentStatus): Promise<void> {
    const db = this.state.getDB();
    const now = Date.now();
    db.query(
      `UPDATE agents SET status = ?, last_action_timestamp = ? WHERE id = ?`,
    ).run(status, now, agentId);
  }

  async getAgent(agentId: string): Promise<AgentRecord | null> {
    const db = this.state.getDB();
    const result = db.query(`SELECT * FROM agents WHERE id = ?`).get(agentId);
    return result ? this.mapRow(result) : null;
  }

  async getRunningAgents(): Promise<AgentRecord[]> {
    const db = this.state.getDB();
    const rows = db
      .query(`SELECT * FROM agents WHERE status = 'working'`)
      .all() as any[];
    return rows.map((row) => this.mapRow(row));
  }

  async getStuckAgents(
    heartbeatThresholdMs: number,
    progressThresholdMs: number,
    gracePeriodMs: number = 30000,
  ): Promise<AgentRecord[]> {
    const db = this.state.getDB();
    const now = Date.now();
    const cutoffHeartbeat = now - heartbeatThresholdMs;
    const cutoffProgress = now - progressThresholdMs;
    const cutoffSpawned = now - gracePeriodMs;
    const rows = db
      .query(
        `SELECT * FROM agents WHERE status = 'working' AND spawned_at < ? AND (
        (last_heartbeat IS NULL OR last_heartbeat < ?) OR
        (last_progress_update IS NULL OR last_progress_update < ?)
      )`,
      )
      .all(cutoffSpawned, cutoffHeartbeat, cutoffProgress) as any[];
    return rows.map((row) => this.mapRow(row));
  }

  async getChildren(parentId: string): Promise<AgentRecord[]> {
    const db = this.state.getDB();
    const rows = db
      .query(`SELECT * FROM agents WHERE parent_id = ?`)
      .all(parentId) as any[];
    return rows.map((row) => this.mapRow(row));
  }

  async deleteAgent(agentId: string): Promise<void> {
    const db = this.state.getDB();
    db.query(`DELETE FROM agents WHERE id = ?`).run(agentId);
  }
}
