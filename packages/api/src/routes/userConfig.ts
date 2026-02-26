/**
 * User config routes for Firmius API
 * REST endpoints for user configuration
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { asyncHandler } from "../middleware/error";
import UserConfigManager from "@firmius/core/config";

type RouteHandler = (req: BunRequest) => Promise<Response>;

/**
 * GET /api/user/config - Get user config
 */
export const getUserConfig: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    const manager = UserConfigManager.getInstance();
    await manager.load();
    const config = manager.get();

    return withCORS(
      new Response(JSON.stringify(config), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/user/config - Save user config
 */
export const updateUserConfig: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    const body = (await req.json()) as {
      defaultModels: Record<string, { providerId: string; modelId: string }>;
    };

    if (!body || typeof body !== "object" || !("defaultModels" in body)) {
      throw new Error("Invalid user config: missing defaultModels");
    }

    const manager = UserConfigManager.getInstance();
    await manager.load();
    manager.set(body);
    await manager.save();

    return withCORS(
      new Response(JSON.stringify(manager.get()), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * Routes dispatcher for user config
 */
export function userConfigRoutes(
  req: BunRequest,
): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/api/user/config" && req.method === "GET") {
    return getUserConfig(req);
  }

  if (path === "/api/user/config" && req.method === "POST") {
    return updateUserConfig(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
