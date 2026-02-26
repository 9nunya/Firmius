/**
 * Purpose routes for Firmius API
 * REST endpoints for purpose discovery
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { asyncHandler } from "../middleware/error";
import { purposeRegistry } from "@firmius/core/registry";

type RouteHandler = (req: BunRequest) => Promise<Response>;

/**
 * GET /api/purposes - List all purpose names
 */
export const listPurposes: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    const purposes = purposeRegistry.listPurposes().map((p) => p.name);

    return withCORS(
      new Response(JSON.stringify(purposes), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * Routes dispatcher for purposes
 */
export function purposeRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/api/purposes" && req.method === "GET") {
    return listPurposes(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
