/**
 * Thread routes for Firmius API
 * REST endpoints for thread lifecycle operations
 */

import type { BunRequest } from "../types/request";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import {
  validateCreateThreadRequest,
  validateThreadId,
} from "../middleware/validation";
import ThreadService from "../services/ThreadService";
import EventService from "../services/EventService";
import type { CreateThreadRequest, ThreadResponse } from "@firmius/shared/api";
import { AgentWorkType } from "@firmius/shared/types";

type RouteHandler = (req: BunRequest) => Promise<Response>;

// ==================== Thread Routes ====================

const buildThreadResponse = (thread: any): ThreadResponse => ({
  id: thread.id,
  title: thread.title ?? "Untitled",
  rootCwd: thread.rootCwd,
  leadAgentId: thread.leadAgent.id,
  checkpointedAt: thread.checkpointedAt,
  agentCount: thread.getSubagents().length + 1,
  tokensLimit: thread.tokensLimit,
  tokensUsed: thread.tokensUsed,
  modelId: (thread.leadAgent as any).context.execution.generationOptions
    ?.modelId,
  providerId: (thread.leadAgent as any).context.execution.generationOptions
    ?.providerId,
  reasoningEffort: (thread.leadAgent as any).context.execution.generationOptions
    ?.reasoningEffort,
  hostType: (thread as any).hostConfig?.type ?? "local",
});

/**
 * GET /api/threads - List all threads
 */
export const listThreads: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    const threads = await ThreadService.listThreads();

    const threadResponses: ThreadResponse[] = threads.map(buildThreadResponse);

    return withCORS(
      new Response(JSON.stringify({ threads: threadResponses }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads - Create new thread
 */
export const createThread: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateCreateThreadRequest(req, async () => new Response());

    const body = req._parsedBody as CreateThreadRequest;

    const thread = await ThreadService.createThread({
      hostConfig: body.hostConfig,
      rootCwd: body.rootCwd,
      purpose: body.purpose,
      objective: body.objective || "Assist the user with their request.",
      workType: body.workType ? AgentWorkType[body.workType as keyof typeof AgentWorkType] : undefined,
      generationOptions: body.generationOptions,
    });

    await ThreadService.saveCheckpoint(thread);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "thread_created",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        { threadId: thread.id, purpose: body.purpose },
      ),
    );

    const response = buildThreadResponse(thread);

    return withCORS(
      new Response(JSON.stringify(response), {
        status: 201,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * GET /api/threads/:id - Get thread details
 */
export const getThread: RouteHandler = asyncHandler(async (req: BunRequest) => {
  await validateThreadId(req, async () => new Response());

  const threadId = req._threadId!;

  const thread = await ThreadService.getThread(threadId);

  if (!thread) {
    throw Errors.notFound(`Thread with id ${threadId} not found`);
  }

  const response = buildThreadResponse(thread);

  return withCORS(
    new Response(JSON.stringify(response), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
});

/**
 * DELETE /api/threads/:id - Delete thread
 */
export const deleteThread: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const deleted = await ThreadService.deleteThread(threadId);

    if (!deleted) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    EventService.clearHistory(threadId);

    return withCORS(
      new Response(JSON.stringify({ message: "Thread deleted successfully" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/resume - Resume thread
 */
export const resumeThread: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    thread.clearInterrupted();

    await ThreadService.saveCheckpoint(thread);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "agent_status_changed",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        { status: "idle", reason: "Thread resumed" },
      ),
    );

    const response = buildThreadResponse(thread);

    return withCORS(
      new Response(JSON.stringify(response), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/interrupt - Interrupt thread
 */
export const interruptThread: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    await thread.interrupt();

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "agent_status_changed",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        { status: "idle", reason: "Thread interrupted" },
      ),
    );

    const response = buildThreadResponse(thread);

    return withCORS(
      new Response(JSON.stringify(response), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * Routes dispatcher for /api/threads
 */
export function threadRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/api/threads") {
    if (req.method === "GET") {
      return listThreads(req);
    } else if (req.method === "POST") {
      return createThread(req);
    }
  }

  const threadMatch = path.match(/^\/api\/threads\/([^/]+)$/);
  if (threadMatch) {
    if (req.method === "GET") {
      return getThread(req);
    } else if (req.method === "DELETE") {
      return deleteThread(req);
    }
  }

  const resumeMatch = path.match(/^\/api\/threads\/([^/]+)\/resume$/);
  if (resumeMatch && req.method === "POST") {
    return resumeThread(req);
  }

  const interruptMatch = path.match(/^\/api\/threads\/([^/]+)\/interrupt$/);
  if (interruptMatch && req.method === "POST") {
    return interruptThread(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
