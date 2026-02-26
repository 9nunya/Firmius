import { z } from "zod";
import type { ITool, ToolContext, IAgent, ToolResult } from "@firmius/shared/types";
import { ToolScope, AgentWorkType, isAgentWorkflow } from "@firmius/shared";
import { Engine } from "@firmius/core";

// Operation-specific output types
export interface ContextInspectOutput {
  id: string;
  parentId?: string;
  subagentIds: string[];
  purpose: string;
  metrics: {
    totalTokens: number;
    lastTurnTokens: number;
    uptime: number;
  };
  lspAvailability: Record<string, { command: string; available: boolean }>;
  activeSubAgents: { id: string; purpose: string; objective: string }[];
}

export interface ContextManageSimpleOutput {
  message: string;
}

// Union type for all possible outputs
export type ContextManageOutput =
  | ContextInspectOutput
  | ContextManageSimpleOutput;

// Discriminated union schema for operation-specific inputs
const ContextManageInputSchema = z.discriminatedUnion("operation", [
  z.object({
    operation: z.literal("inspect"),
  }),
  z.object({
    operation: z.literal("mark_anchor"),
    decision: z
      .string()
      .describe(
        "A concise description of the critical decision made (e.g., 'Selected PostgreSQL as database', 'Chose Docker for deployment')."
      ),
  }),
  z.object({
    operation: z.literal("set_limit"),
    limit: z
      .number()
      .describe("Number of reasoning turns to show in system prompt (0-10, default 4)"),
  }),
]);

export type ContextManageInput = z.infer<typeof ContextManageInputSchema>;

// Execute inspect operation
async function executeInspect(context: ToolContext): Promise<ToolResult<ContextManageOutput>> {
  const subAgents = Array.from(Engine.agentFactory.agents.values() as Iterable<IAgent>)
    .filter((a) => a.context?.identity?.parentId === context.agent.identity.id)
    .map((a) => ({
      id: a.id,
      purpose: a.context?.identity?.purpose ?? "",
      objective: a.context?.identity?.objective ?? "",
    }));

  const lsp = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());
  const availability = await lsp.getAvailability();

  const output: ContextInspectOutput = {
    id: context.agent.identity.id,
    parentId: context.agent.identity.parentId,
    subagentIds: context.agent.identity.subagentIds,
    purpose: context.agent.identity.purpose,
    metrics: {
      totalTokens: context.agent.state.metrics.totalTokens,
      lastTurnTokens: context.agent.state.metrics.lastTurnTokens,
      uptime: Math.floor((Date.now() - context.agent.state.metrics.startTime) / 1000),
    },
    lspAvailability: availability,
    activeSubAgents: subAgents,
  };

  return {
    success: true,
    summary: `Inspected context of agent ${context.agent.identity.id}`,
    output
  };
}

// Execute mark_anchor operation
async function executeMarkAnchor(
  decision: string,
  context: ToolContext
): Promise<ToolResult<ContextManageOutput>> {
  const agent = Engine.agentFactory.agents.get(context.agent.identity.id);
  if (!agent || !agent.context) {
    return { success: false, summary: "Agent not found", error: "Agent not found" };
  }

  agent.context.execution.anchors.add(decision);

  if (agent.context.historyData?.history?.type === AgentWorkType.Goal && isAgentWorkflow(agent.context.historyData.history.workflow)) {
    const workflow = agent.context.historyData.history.workflow;
    if (workflow.turns.length > 0) {
      const lastTurn = workflow.turns[workflow.turns.length - 1];
      if (lastTurn) {
        lastTurn.protected = true;
      }
    }
  } else if (agent.context.historyData?.history?.type === AgentWorkType.Conversational && agent.context.historyData.history.conversation) {
    const history = agent.context.historyData.history.conversation.history;
    if (history.length > 0) {
      const lastEntry = history[history.length - 1];
      if (lastEntry && isAgentWorkflow(lastEntry)) {
        const lastTurn = lastEntry.turns[lastEntry.turns.length - 1];
        if (lastTurn) {
          lastTurn.protected = true;
        }
      }
    }
  }

  return { success: true, summary: `Anchor marked: "${decision}"`, output: { message: `Anchor stored: "${decision}"` } };
}

// Execute set_limit operation
async function executeSetLimit(
  limit: number,
  context: ToolContext
): Promise<ToolResult<ContextManageOutput>> {
  const clampedLimit = Math.min(Math.max(limit, 0), 10);
  context.agent.historyData.reasoningHistoryLimit = clampedLimit;
  return {
    success: true,
    summary: `Reasoning history limit set to ${clampedLimit}`,
    output: { message: `Limit set to ${clampedLimit}` }
  };
}

export const ContextManageTool: ITool<ContextManageInput, ContextManageOutput> = {
  metadata: {
    name: "context_manage",
    description: "Manage agent context.",
    scope: ToolScope.Orchestration,
  },

  input: ContextManageInputSchema,

  summarizeInput: (input: ContextManageInput) => {
    return `context_manage: ${input.operation}`;
  },

  execute: async (input: ContextManageInput, context: ToolContext): Promise<ToolResult<ContextManageOutput>> => {
    switch (input.operation) {
      case "inspect":
        return executeInspect(context);
      case "mark_anchor":
        return executeMarkAnchor(input.decision, context);
      case "set_limit":
        return executeSetLimit(input.limit, context);
      default:
        return { success: false, summary: "Invalid operation", error: "Invalid operation" };
    }
  },
};

export const AllContextTools = [ContextManageTool];
