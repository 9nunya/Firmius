/**
 * Provider routes for Firmius API
 * REST endpoints for provider and model discovery
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { Engine } from "@firmius/core";
import ThreadService from "../services/ThreadService";
import { validateThreadId } from "../middleware/validation";

type RouteHandler = (req: BunRequest) => Promise<Response>;

/**
 * GET /api/providers - List all providers and their models
 */
export const listProviders: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    const providers = Object.entries(Engine.providers).map(([id, provider]) => {
      return {
        id,
        name: id,
        type: provider.type,
        models: provider.listModels().map((model) => ({
          id: model.name,
          name: model.name,
          ctx: model.ctx,
          capabilities: model.capabilities,
          modalities: model.modalities,
          reasoning: model.reasoning,
        })),
      };
    });

    return withCORS(
      new Response(JSON.stringify({ providers }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/settings - Update thread generation options
 */
export const updateThreadSettings: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;
    const thread = await ThreadService.getThread(threadId);

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    const body = (await req.json()) as {
      generationOptions: {
        modelId?: string;
        providerId?: string;
        reasoningEffort?: "none" | "minimal" | "low" | "medium" | "high";
      };
    };

    if (!body.generationOptions) {
      throw Errors.badRequest("generationOptions is required");
    }

    // Update all agents in the thread to use the new settings
    const leadAgent = thread.leadAgent;
    const subagents = thread.getSubagents();
    const allAgents = [leadAgent, ...subagents].filter(
      (agent) => agent !== undefined && agent !== null,
    );

    for (const agent of allAgents) {
      if (!agent.context) continue;
      const currentOptions = agent.context.execution.generationOptions;
      // Update individual properties instead of replacing the whole object
      if (body.generationOptions.providerId !== undefined) {
        (currentOptions as any).providerId = body.generationOptions.providerId;
      }
      if (body.generationOptions.modelId !== undefined) {
        (currentOptions as any).modelId = body.generationOptions.modelId;
      }
      if (body.generationOptions.reasoningEffort !== undefined) {
        (currentOptions as any).reasoningEffort =
          body.generationOptions.reasoningEffort;
      }
    }

    // Save checkpoint
    await ThreadService.saveCheckpoint(thread);

    return withCORS(
      new Response(
        JSON.stringify({ message: "Settings updated successfully" }),
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
 * Routes dispatcher for providers and settings
 */
export function providerRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/api/providers" && req.method === "GET") {
    return listProviders(req);
  }

  const settingsMatch = path.match(/^\/api\/threads\/([^/]+)\/settings$/);
  if (settingsMatch && req.method === "POST") {
    return updateThreadSettings(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
