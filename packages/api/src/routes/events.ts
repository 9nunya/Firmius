/**
 * Event routes for Firmius API
 * REST endpoints for event polling (SSE fallback)
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { asyncHandler } from "../middleware/error";
import { validateThreadId } from "../middleware/validation";
import EventService from "../services/EventService";
import ThreadService from "../services/ThreadService";
import type { SSEMessage, EventHistoryResponse } from "@firmius/shared/sse";

type RouteHandler = (req: BunRequest) => Promise<Response>;

// ==================== Event Routes ====================

/**
 * GET /api/threads/:id/events?since= - Poll events (SSE fallback)
 *
 * Query parameters:
 * - since: Optional timestamp in ISO format or epoch milliseconds
 *
 * Returns events that occurred after the specified timestamp.
 * Used as a fallback when SSE is not available.
 */
export const pollEvents: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const url = new URL(req.url);
    const sinceParam = url.searchParams.get("since");
    let since: Date | undefined;

    if (sinceParam) {
      try {
        const parsed = new Date(sinceParam);
        if (!isNaN(parsed.getTime())) {
          since = parsed;
        } else {
          const epoch = parseInt(sinceParam, 10);
          if (!isNaN(epoch)) {
            since = new Date(epoch);
          }
        }
      } catch (error) {
        // Invalid 'since' parameter - ignore and return all events
      }
    }

    const events: SSEMessage[] = await EventService.getEventHistory(
      threadId,
      since,
    );

    const thread = await ThreadService.getThread(threadId);
    const agents: any[] = thread ? [
      thread.leadAgent,
      ...thread.getSubagents()
    ].map((agent: any) => ({
      id: agent.id,
      name: agent.readableName || agent.id,
      purpose: agent.context?.identity?.purpose || "General",
      readableName: agent.readableName || agent.id,
      parentId: agent.context?.identity?.parentId,
      isLead: agent.context?.identity?.parentId === undefined,
      turnCount: agent.getTurnCount?.() ?? 1,
      objective: agent.context?.identity?.objective || "",
      subagentIds: agent.context?.identity?.subagentIds || [],
      status: agent.status || 'idle',
      modelId: agent.getModelInfo?.()?.id,
      threadId: thread.id
    })) : [];

    const response: EventHistoryResponse = {
      events,
      agents,
      timestamp: new Date(),
    };

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
 * Routes dispatcher for /api/threads/:id/events
 */
export function eventRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  const eventsMatch = path.match(/^\/api\/threads\/([^/]+)\/events$/);
  if (eventsMatch && req.method === "GET") {
    return pollEvents(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
