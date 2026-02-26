/**
 * Fleet routes for Firmius API
 * REST endpoints for fleet management operations
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { validateThreadId } from "../middleware/validation";
import { Engine } from "@firmius/core";
import type { ICoordinator } from "@firmius/shared/types";

type RouteHandler = (req: BunRequest) => Promise<Response>;

// ==================== Fleet Routes ====================

/**
 * GET /api/threads/:threadId/fleet/status - Get full fleet status
 */
export const getFleetStatus: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const thread = Engine.getThread(req._threadId!);
    if (!thread) {
      throw Errors.notFound("Thread not found");
    }
    const coordinator = thread.coordinator as ICoordinator;
    const status = await coordinator.getFleetStatus();

    return withCORS(
      new Response(JSON.stringify(status), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * GET /api/threads/:threadId/fleet/agents - List all fleet agents
 */
export const listFleetAgents: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const thread = Engine.getThread(req._threadId!);
    if (!thread) {
      throw Errors.notFound("Thread not found");
    }
    const coordinator = thread.coordinator as ICoordinator;
    const status = await coordinator.getFleetStatus();

    return withCORS(
      new Response(
        JSON.stringify({
          agents: status.agents,
          count: status.agents.length,
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
 * GET /api/threads/:threadId/fleet/tasks - List all tasks (deprecated - tasks no longer tracked)
 */
export const listFleetTasks: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    return withCORS(
      new Response(
        JSON.stringify({
          tasks: [],
          count: 0,
          message:
            "Tasks are no longer tracked - agents manage their own work via todo and MD files",
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
 * POST /api/threads/:threadId/fleet/nudge - Nudge an agent
 */
export const nudgeFleetAgent: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const body = await req.json();
    const { agentId, message } = body;

    if (!agentId || !message) {
      throw Errors.badRequest("agentId and message are required");
    }

    const thread = Engine.getThread(req._threadId!);
    if (!thread) {
      throw Errors.notFound("Thread not found");
    }
    const coordinator = thread.coordinator as ICoordinator;
    await coordinator.nudgeAgent(agentId, message);

    return withCORS(
      new Response(JSON.stringify({ success: true }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:threadId/fleet/kill - Kill an agent
 */
export const killFleetAgent: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const body = await req.json();
    const { agentId, reason } = body;

    if (!agentId) {
      throw Errors.badRequest("agentId is required");
    }

    const thread = Engine.getThread(req._threadId!);
    if (!thread) {
      throw Errors.notFound("Thread not found");
    }
    const coordinator = thread.coordinator as ICoordinator;
    await coordinator.killAgent(agentId);

    return withCORS(
      new Response(
        JSON.stringify({ success: true, reason: reason ?? "Killed via API" }),
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
 * Routes dispatcher for /api/threads/:threadId/fleet/*
 */
export function fleetRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  const fleetStatusMatch = path.match(
    /^\/api\/threads\/([^/]+)\/fleet\/status$/,
  );
  if (fleetStatusMatch && req.method === "GET") {
    return getFleetStatus(req);
  }

  const fleetAgentsMatch = path.match(
    /^\/api\/threads\/([^/]+)\/fleet\/agents$/,
  );
  if (fleetAgentsMatch && req.method === "GET") {
    return listFleetAgents(req);
  }

  const fleetTasksMatch = path.match(/^\/api\/threads\/([^/]+)\/fleet\/tasks$/);
  if (fleetTasksMatch && req.method === "GET") {
    return listFleetTasks(req);
  }

  const fleetNudgeMatch = path.match(/^\/api\/threads\/([^/]+)\/fleet\/nudge$/);
  if (fleetNudgeMatch && req.method === "POST") {
    return nudgeFleetAgent(req);
  }

  const fleetKillMatch = path.match(/^\/api\/threads\/([^/]+)\/fleet\/kill$/);
  if (fleetKillMatch && req.method === "POST") {
    return killFleetAgent(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Not found" }), {
      status: 404,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
