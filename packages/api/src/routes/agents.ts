/**
 * Agent routes for Firmius API
 * REST endpoints for agent hierarchy and status operations
 */

import type { BunRequest } from "../types/request";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { validateThreadId, validateAgentId } from "../middleware/validation";
import ThreadService from "../services/ThreadService";
import EventService from "../services/EventService";
import type { AgentResponse, AgentHierarchy } from "@firmius/shared/api";
import { snapshotStorage } from "@firmius/core/services/SnapshotStorage";
import { writeFile } from "node:fs/promises";

type RouteHandler = (req: BunRequest) => Promise<Response>;

// ==================== Helper Functions ====================

/**
 * Builds agent hierarchy tree from agents list
 */
function buildAgentHierarchy(
  agents: AgentResponse[],
  parentId?: string,
): AgentHierarchy[] {
  const children = agents.filter((agent) => agent.parentId === parentId);

  return children.map((agent) => ({
    agent,
    children: buildAgentHierarchy(agents, agent.id),
  }));
}

/**
 * Maps engine agent to API response format
 */
function mapEngineAgentToResponse(
  engineAgent: any,
  threadId: string,
  leadAgentId: string,
): AgentResponse {
  const isLead = leadAgentId === engineAgent.id;
  return {
    id: engineAgent.id,
    purpose: engineAgent.context?.identity?.purpose || engineAgent.purpose || "General",
    readableName: engineAgent.readableName ?? engineAgent.id,
    parentId: engineAgent.context?.identity?.parentId || engineAgent.parentId,
    isLead,
    turnCount: engineAgent.getTurnCount?.() ?? engineAgent.turnCount ?? 1,
    objective: engineAgent.context?.identity?.objective || engineAgent.objective || "",
    subagentIds: engineAgent.context?.identity?.subagentIds || engineAgent.subagentIds || [],
    status: engineAgent.status || 'idle',
    modelId: engineAgent.getModelInfo?.()?.id || engineAgent.modelId,
    threadId,
    tokensUsed: engineAgent.context?.state?.metrics?.totalTokens || engineAgent.tokensUsed || 0,
  };
}

// ==================== Agent Routes ====================

/**
 * GET /api/threads/:id/agents - List agents with hierarchy
 */
export const listAgents: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    const engineAgents = [thread.leadAgent, ...thread.getSubagents()];
    const leadAgentId = thread.leadAgent.id;

    const agents: AgentResponse[] = engineAgents.map((agent) =>
      mapEngineAgentToResponse(agent, threadId, leadAgentId),
    );

    const hierarchy: AgentHierarchy[] = buildAgentHierarchy(agents);

    return withCORS(
      new Response(
        JSON.stringify({
          agents,
          hierarchy,
          count: agents.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * GET /api/agents/:id - Get agent details
 */
export const getAgent: RouteHandler = asyncHandler(async (req: BunRequest) => {
  await validateAgentId(req, async () => new Response());

  const agentId = req._agentId!;

  const threads = await ThreadService.listThreads();
  let foundAgent: AgentResponse | null = null;

  for (const thread of threads) {
    const engineAgents = [thread.leadAgent, ...thread.getSubagents()];
    const leadAgentId = thread.leadAgent.id;
    const agent = engineAgents.find((a) => a.id === agentId);

    if (agent) {
      foundAgent = mapEngineAgentToResponse(agent, thread.id, leadAgentId);
      break;
    }
  }

  if (!foundAgent) {
    throw Errors.notFound(`Agent with id ${agentId} not found`);
  }

  return withCORS(
    new Response(JSON.stringify(foundAgent), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
});

/**
 * GET /api/threads/:threadId/agents/:agentId/history - Get agent journal entries
 */
export const getAgentHistory: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;
    const url = new URL(req.url);
    const match = url.pathname.match(
      /\/api\/threads\/[^/]+\/agents\/([^/]+)\/history$/,
    );
    const agentId = match?.[1];

    if (!agentId) {
      throw Errors.badRequest("Agent ID is required");
    }

    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    const entries = await thread.getAgentJournalEntries(agentId);

    return withCORS(
      new Response(
        JSON.stringify({
          agentId,
          threadId,
          entries,
          count: entries.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * GET /api/threads/:threadId/agents/:agentId/todos - Get agent todo list
 */
export const getAgentTodos: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;
    const url = new URL(req.url);
    const match = url.pathname.match(
      /\/api\/threads\/[^/]+\/agents\/([^/]+)\/todos$/,
    );
    const agentId = match?.[1];

    if (!agentId) {
      throw Errors.badRequest("Agent ID is required");
    }

    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    // Find the agent in the thread
    const engineAgents = [thread.leadAgent, ...thread.getSubagents()];
    const agent = engineAgents.find((a) => a.id === agentId);

    let todos: any[] = [];
    if (agent) {
      todos = agent.context?.state?.todos || [];
    } else {
      // Agent not in active memory - check if it's a valid agent that existed
      // Return empty todos instead of 404 (agent may have terminated)
      const isKnownAgent = thread.getAllAgentIds().includes(agentId);
      if (!isKnownAgent) {
        throw Errors.notFound(`Agent with id ${agentId} not found in thread`);
      }
    }

    return withCORS(
      new Response(
        JSON.stringify({
          agentId,
          threadId,
          todos,
          count: todos.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * Routes dispatcher for /api/threads/:id/agents and /api/agents/:id
 */
export function agentRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  const agentTodosMatch = path.match(
    /^\/api\/threads\/([^/]+)\/agents\/([^/]+)\/todos$/,
  );
  if (agentTodosMatch && req.method === "GET") {
    return getAgentTodos(req);
  }

  const agentHistoryMatch = path.match(
    /^\/api\/threads\/([^/]+)\/agents\/([^/]+)\/history$/,
  );
  if (agentHistoryMatch && req.method === "GET") {
    return getAgentHistory(req);
  }

  const threadAgentsMatch = path.match(/^\/api\/threads\/([^/]+)\/agents$/);
  if (threadAgentsMatch && req.method === "GET") {
    return listAgents(req);
  }

  const agentMatch = path.match(/^\/api\/agents\/([^/]+)$/);
  if (agentMatch && req.method === "GET") {
    return getAgent(req);
  }

  // Model switching endpoint
  const agentModelMatch = path.match(
    /^\/api\/threads\/([^/]+)\/agents\/([^/]+)\/model$/,
  );
  if (agentModelMatch && req.method === "POST") {
    return updateAgentModel(req);
  }

  // Undo last turn endpoint
  const agentUndoMatch = path.match(
    /^\/api\/threads\/([^/]+)\/agents\/([^/]+)\/undo-turn$/,
  );
  if (agentUndoMatch && req.method === "POST") {
    return undoLastTurn(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}

/**
 * POST /api/threads/:threadId/agents/:agentId/model
 * Update agent model (takes effect after current turn)
 */
export const updateAgentModel: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());
    await validateAgentId(req, async () => new Response());

    const threadId = req._threadId!;
    const agentId = req._agentId!;

    const body = (await req.json()) as {
      modelId?: string;
      providerId?: string;
      reasoningEffort?: string;
      maxTokens?: number;
    };

    const thread = await ThreadService.getThread(threadId);
    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    // Find agent in thread
    const engineAgents = [thread.leadAgent, ...thread.getSubagents()];
    const agent = engineAgents.find((a) => a.id === agentId);

    if (!agent) {
      throw Errors.notFound(`Agent with id ${agentId} not found`);
    }

    // Update generation options - takes effect after current turn
    agent.updateGenerationOptions({
      modelId: body.modelId,
      providerId: body.providerId,
      reasoningEffort: body.reasoningEffort,
      maxTokens: body.maxTokens,
    });

    return withCORS(
      new Response(
        JSON.stringify({
          message: "Model updated successfully",
          agentId,
          newModelId: body.modelId,
          status: "queued",
          note: "Change takes effect after current turn completes",
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * POST /api/threads/:threadId/agents/:agentId/undo-turn
 * Undo the last turn for a specific agent
 */
export const undoLastTurn: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());
    await validateAgentId(req, async () => new Response());

    const threadId = req._threadId!;
    const agentId = req._agentId!;

    const thread = await ThreadService.getThread(threadId);
    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    // Find agent
    const engineAgents = [thread.leadAgent, ...thread.getSubagents()];
    const agent = engineAgents.find((a) => a.id === agentId);

    if (!agent) {
      throw Errors.notFound(`Agent with id ${agentId} not found`);
    }

    // Get agent's current turn count
    const currentTurn = agent.getTurnCount();
    if (currentTurn <= 0) {
      throw Errors.badRequest("Agent has no turns to undo");
    }

    // Revert file changes for this turn using timestamps for accuracy
    const agentJournal = await (thread as any).getOrCreateAgentJournal(agentId);
    const lastTurnSeq = await agentJournal.getLastAgentTurn(agentId);
    let targetTimestamp = 0;
    if (lastTurnSeq !== null) {
      const entries = await agentJournal.readAllEntries();
      const lastTurnEntry = entries.find((e: any) => e.sequence === lastTurnSeq);
      if (lastTurnEntry) targetTimestamp = lastTurnEntry.timestamp;
    }

    // Get all snapshots for this thread
    const allSnapshots = await snapshotStorage.getAllSnapshotsForThread(threadId);
    const restoredFiles: string[] = [];

    // Find file edits from the last turn and restore them
    for (const snapshot of allSnapshots) {
      // If we have a timestamp, use it. Otherwise fall back to turnIndex comparison.
      const match = targetTimestamp > 0
        ? snapshot.timestamp >= targetTimestamp
        : (snapshot.agentId === agentId && snapshot.turnIndex === currentTurn);

      if (match) {
        try {
          if (snapshot.beforeContent !== null) {
            await writeFile(snapshot.file, snapshot.beforeContent, 'utf8');
            restoredFiles.push(snapshot.file);
          }
        } catch (err) {
          console.error(`Failed to restore file ${snapshot.file}:`, err);
        }
      }
    }

    // Forget the last turn from agent's journal
    await agent.forgetLastTurn();

    // Clean up persisted events for this turn
    await EventService.removeEventsForTurn(threadId, agentId, currentTurn);

    // Interrupt agent if it's currently active
    if (agent.status === 'working') {
      await agent.interrupt();
    }

    return withCORS(
      new Response(
        JSON.stringify({
          message: "Last turn undone successfully",
          agentId,
          undoneTurn: currentTurn,
          restoredFiles,
          restoredCount: restoredFiles.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);
