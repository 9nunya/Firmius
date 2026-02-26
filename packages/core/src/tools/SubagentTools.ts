import { z } from "zod";
import type { ITool, ToolContext, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";
import { Engine } from "@firmius/core";

// =============================================================================
// SCHEMAS
// =============================================================================

const SubagentWaitInputSchema = z.object({
  agent: z.string().describe("Agent ID or readable name to wait for"),
});

const SubagentPollInputSchema = z.object({
  agent: z
    .string()
    .optional()
    .describe(
      "Agent ID or readable name to check (optional, if omitted return all)",
    ),
});

const SubagentNudgeInputSchema = z.object({
  agent: z.string().describe("Agent ID or readable name to nudge"),
  message: z.string().optional().describe("Nudge message content"),
  forceRestart: z
    .boolean()
    .default(false)
    .describe("Force restart even if agent is working"),
});

const SubagentStatusInputSchema = z.object({
  agent: z
    .string()
    .optional()
    .describe(
      "Agent ID or readable name to check (optional, if omitted returns self)",
    ),
});

const SubagentKillInputSchema = z.object({
  agent: z.string().describe("Agent ID or readable name to kill"),
  reason: z.string().optional().describe("Reason for termination"),
});

type SubagentWaitInput = z.infer<typeof SubagentWaitInputSchema>;
type SubagentPollInput = z.infer<typeof SubagentPollInputSchema>;
type SubagentNudgeInput = z.infer<typeof SubagentNudgeInputSchema>;
type SubagentStatusInput = z.infer<typeof SubagentStatusInputSchema>;
type SubagentKillInput = z.infer<typeof SubagentKillInputSchema>;

interface SubagentWaitOutput {
  agentId: string;
  status: string;
  elapsedMs: number;
}

interface SubagentPollOutput {
  agents: Array<{
    id: string;
    status: string;
    timeSinceLastTurnMs?: number;
    timeSinceLastToolCallMs?: number;
  }>;
}

interface SubagentNudgeOutput {
  action: string;
  agentId: string;
}

interface SubagentStatusOutput {
  agent: {
    id: string;
    readableName: string;
    purpose: string;
    objective: string;
    status: string;
    lastHeartbeat: number | null;
    lastProgressUpdate: number | null;
    timeSinceHeartbeatMs: number | null;
    timeSinceProgressMs: number | null;
    stuck: boolean;
    stuckReason: string | null;
  } | null;
}

interface SubagentKillOutput {
  agentId: string;
}

// =============================================================================
// UTILITIES
// =============================================================================

async function resolveAgentIdentifier(
  identifier: string,
  parentAgentId?: string,
): Promise<string> {
  const directMatch = Engine.agentFactory.agents.get(identifier);
  if (directMatch) return directMatch.id;

  if (parentAgentId) {
    const parent = Engine.agentFactory.agents.get(parentAgentId);
    if (parent) {
      const childIds = parent.identity.subagentIds;
      for (const childId of childIds) {
        const child = Engine.agentFactory.agents.get(childId);
        if (child?.readableName.toLowerCase().includes(identifier.toLowerCase())) return childId;
      }
      throw new Error(`Agent does not exist: ${identifier}`);
    }
  }

  for (const [agentId, agent] of Engine.agentFactory.agents) {
    if (agent.readableName.toLowerCase().includes(identifier.toLowerCase())) return agentId;
  }

  throw new Error(`Agent does not exist: ${identifier}`);
}

// =============================================================================
// TOOLS
// =============================================================================

export const SubagentWaitTool: ITool<SubagentWaitInput, SubagentWaitOutput> = {
  metadata: {
    name: "subagent_wait",
    description: `Wait for your child agent to finish.`,
    scope: ToolScope.Delegation,
  },
  input: SubagentWaitInputSchema,
  execute: async (
    input: SubagentWaitInput,
    context: ToolContext,
  ): Promise<ToolResult<SubagentWaitOutput>> => {
    try {
      const targetAgentId = await resolveAgentIdentifier(input.agent, context.agent.identity.id);
      const startTime = Date.now();
      while (true) {
        const childAgent = Engine.agentFactory.agents.get(targetAgentId);
        if (!childAgent) return { success: false, summary: "Not found", error: `Child agent ${targetAgentId} does not exist` };
        if (childAgent.status === "idle" || childAgent.status === "completed") {
          return {
            success: true,
            summary: `Agent ${targetAgentId} finished (${childAgent.status})`,
            output: { agentId: targetAgentId, status: childAgent.status, elapsedMs: Date.now() - startTime }
          };
        }
        await new Promise((resolve) => setTimeout(resolve, 500));
      }
    } catch (error: any) {
      return { success: false, summary: "Wait failed", error: error.message };
    }
  },
  summarizeInput: (input) => `wait for ${input.agent}`,
};

export const SubagentPollTool: ITool<SubagentPollInput, SubagentPollOutput> = {
  metadata: {
    name: "subagent_poll",
    description: `Check status of child agents.`,
    scope: ToolScope.Delegation,
  },
  input: SubagentPollInputSchema,
  execute: async (
    input: SubagentPollInput,
    context: ToolContext,
  ): Promise<ToolResult<SubagentPollOutput>> => {
    const parentId = context.agent.identity.id;
    const parentAgent = Engine.agentFactory.agents.get(parentId);
    if (!parentAgent) return { success: false, summary: "Parent not found", error: "Parent agent not found" };

    const subagentIds = parentAgent.identity.subagentIds;
    const getTimingInfo = (child: any) => {
      const now = Date.now();
      const history = child.historyData.history;
      if (history.workflow && history.workflow.turns.length > 0) {
        const lastTurn = history.workflow.turns[history.workflow.turns.length - 1];
        return { timeSinceLastTurnMs: now - lastTurn.timestamp, timeSinceLastToolCallMs: now - lastTurn.timestamp };
      }
      return {};
    };

    const targetIds = input.agent ? subagentIds.filter(id => id === input.agent || Engine.agentFactory.agents.get(id)?.readableName.toLowerCase().includes(input.agent!.toLowerCase())) : subagentIds;
    const agents = targetIds.map(id => {
      const child = Engine.agentFactory.agents.get(id);
      return child ? { id, status: child.status, ...getTimingInfo(child) } : null;
    }).filter((a): a is any => a !== null);

    return { success: true, summary: `Polled ${agents.length} agents`, output: { agents } };
  },
  summarizeInput: (input) => `poll ${input.agent || 'all'}`,
};

export const SubagentNudgeTool: ITool<SubagentNudgeInput, SubagentNudgeOutput> = {
  metadata: {
    name: "subagent_nudge",
    description: `Nudge a subagent.`,
    scope: ToolScope.Delegation,
  },
  input: SubagentNudgeInputSchema,
  execute: async (
    input: SubagentNudgeInput,
    context: ToolContext,
  ): Promise<ToolResult<SubagentNudgeOutput>> => {
    try {
      const targetAgentId = await resolveAgentIdentifier(input.agent, context.agent.identity.id);
      const targetAgent = Engine.agentFactory.agents.get(targetAgentId);
      if (!targetAgent) return { success: false, summary: "Not found", error: "Agent not found", output: { action: "none", agentId: input.agent } };

      const fleet = await context.coordinator.fleet.getAgent(targetAgentId);
      if (!fleet) return { success: false, summary: "Fleet error", error: "Fleet record not found", output: { action: "none", agentId: targetAgentId } };

      const now = Date.now();
      const isStuck = (fleet.lastHeartbeat ? now - fleet.lastHeartbeat > 120000 : true) || (fleet.lastProgressUpdate ? now - fleet.lastProgressUpdate > 300000 : true) || input.forceRestart;

      if (isStuck) {
        await targetAgent.interrupt();
        targetAgent.actUntilAgentEnds(); // fire and forget restart
        return { success: true, summary: `Restarted agent ${input.agent}`, output: { action: "restarted", agentId: targetAgentId } };
      }

      return { success: true, summary: `Nudged agent ${input.agent}`, output: { action: "nudged", agentId: targetAgentId } };
    } catch (error: any) {
      return { success: false, summary: "Nudge failed", error: error.message };
    }
  },
  summarizeInput: (input) => `nudge ${input.agent}`,
};

export const SubagentStatusTool: ITool<SubagentStatusInput, SubagentStatusOutput> = {
  metadata: {
    name: "subagent_status",
    description: `Get status of a subagent.`,
    scope: ToolScope.Delegation,
  },
  input: SubagentStatusInputSchema,
  execute: async (
    input: SubagentStatusInput,
    context: ToolContext,
  ): Promise<ToolResult<SubagentStatusOutput>> => {
    const agentId = input.agent || context.agent.identity.id;
    try {
      const targetAgentId = await resolveAgentIdentifier(agentId);
      const targetAgent = Engine.agentFactory.agents.get(targetAgentId);
      if (!targetAgent) return { success: false, summary: "Not found", error: "Agent not found" };

      const fleet = await context.coordinator.fleet.getAgent(targetAgentId);
      const now = Date.now();
      const lastHeartbeat = fleet?.lastHeartbeat ?? null;
      const lastProgress = fleet?.lastProgressUpdate ?? null;
      const stuck = (lastHeartbeat ? now - lastHeartbeat > 120000 : true) || (lastProgress ? now - lastProgress > 300000 : true);

      return {
        success: true,
        summary: `Status of ${agentId}: ${targetAgent.status}`,
        output: {
          agent: {
            id: targetAgentId,
            readableName: targetAgent.readableName,
            purpose: targetAgent.identity.purpose,
            objective: targetAgent.identity.objective,
            status: targetAgent.status,
            lastHeartbeat,
            lastProgressUpdate: lastProgress,
            timeSinceHeartbeatMs: lastHeartbeat ? now - lastHeartbeat : null,
            timeSinceProgressMs: lastProgress ? now - lastProgress : null,
            stuck,
            stuckReason: stuck ? "Stale state" : null
          }
        }
      };
    } catch (error: any) {
      return { success: false, summary: "Status failed", error: error.message };
    }
  },
  summarizeInput: (input) => `status ${input.agent || 'self'}`,
};

export const SubagentKillTool: ITool<SubagentKillInput, SubagentKillOutput> = {
  metadata: {
    name: "subagent_kill",
    description: `Terminate a subagent.`,
    scope: ToolScope.Delegation,
  },
  input: SubagentKillInputSchema,
  execute: async (
    input: SubagentKillInput,
    context: ToolContext,
  ): Promise<ToolResult<SubagentKillOutput>> => {
    try {
      const targetAgentId = await resolveAgentIdentifier(input.agent, context.agent.identity.id);
      await Engine.agentFactory.terminate(targetAgentId);
      return { success: true, summary: `Killed agent ${input.agent}`, output: { agentId: targetAgentId } };
    } catch (error: any) {
      return { success: false, summary: "Kill failed", error: error.message };
    }
  },
  summarizeInput: (input) => `kill ${input.agent}`,
};

export const AllSubagentTools = [SubagentWaitTool, SubagentPollTool, SubagentNudgeTool, SubagentStatusTool, SubagentKillTool];
