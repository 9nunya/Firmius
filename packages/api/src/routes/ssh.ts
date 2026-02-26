/**
 * SSH config routes for Firmius API
 * Provides user's SSH configurations from ~/.ssh/config
 */

import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import type { BunRequest } from "../types/index";

type RouteHandler = (req: BunRequest) => Promise<Response>;

interface SSHHostConfig {
  id: string;
  alias: string;
  host: string;
  username?: string;
  port?: number;
}

// ==================== SSH Config Parsing ====================

/**
 * Parses an SSH config file and extracts Host entries
 * Supports basic Host, HostName, User, Port, IdentityFile directives
 * Ignores Include directives for simplicity
 */
function parseSSHConfig(content: string): SSHHostConfig[] {
  const configs: SSHHostConfig[] = [];
  const lines = content.split("\n");

  let currentHost: string | null = null;
  let hostData: Partial<SSHHostConfig> = {};

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i]?.trim() ?? "";

    // Skip comments and empty lines
    if (line.startsWith("#") || line.length === 0) {
      continue;
    }

    // Check for Host directive (starts a new host entry)
    if (/^Host\s+/i.test(line)) {
      // Save previous host if exists
      if (currentHost !== null && hostData.host !== undefined) {
        configs.push({
          id: currentHost,
          alias: currentHost,
          host: hostData.host,
          username: hostData.username,
          port: hostData.port,
        });
      }

      // Start new host entry - take first token after "Host"
      const parts = line.split(/\s+/);
      // parts[0] is "Host", parts[1] is the first alias
      currentHost = parts[1] ?? null;
      hostData = {};
      continue;
    }

    // For lines within a host entry (indented or after Host line)
    // Parse key-value pairs
    if (currentHost !== null) {
      const match = line.match(/^(\w+)\s+(.+)$/);
      if (match) {
        const key = match[1];
        const value = match[2];
        if (!key || !value) continue;
        switch (key.toLowerCase()) {
          case "hostname":
            hostData.host = value.trim();
            break;
          case "user":
            hostData.username = value.trim();
            break;
          case "port":
            const portNum = parseInt(value, 10);
            if (!isNaN(portNum)) {
              hostData.port = portNum;
            }
            break;
          case "identityfile":
            // We don't expose identity files (security), but keeping parser simple
            break;
        }
      }
    }
  }

  // Save the last host entry
  if (currentHost !== null && hostData.host !== undefined) {
    configs.push({
      id: currentHost,
      alias: currentHost,
      host: hostData.host,
      username: hostData.username,
      port: hostData.port,
    });
  }

  return configs;
}

// ==================== Route Handler ====================

/**
 * GET /api/ssh-configs
 * Returns list of SSH host configurations from ~/.ssh/config
 */
export const getSSHConfigs: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    if (req.method !== "GET") {
      // Method not allowed - return 405
      return withCORS(
        new Response(JSON.stringify({ error: "Method not allowed" }), {
          status: 405,
          headers: { "Content-Type": "application/json" },
        }),
        req,
      );
    }

    // Get user's home directory (works on Linux/macOS/Windows)
    const homeDir = process.env.HOME || process.env.USERPROFILE || "";
    const sshConfigPath = `${homeDir}/.ssh/config`;

    // Check if file exists
    const file = Bun.file(sshConfigPath);
    const exists = await file.exists();

    if (!exists) {
      return withCORS(
        new Response(JSON.stringify({ configs: [] }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        }),
        req,
      );
    }

    try {
      const content = await file.text();

      if (!content.trim()) {
        return withCORS(
          new Response(JSON.stringify({ configs: [] }), {
            status: 200,
            headers: { "Content-Type": "application/json" },
          }),
          req,
        );
      }

      const configs = parseSSHConfig(content);

      return withCORS(
        new Response(JSON.stringify({ configs }), {
          status: 200,
          headers: { "Content-Type": "application/json" },
        }),
        req,
      );
    } catch (error) {
      console.error("Failed to read or parse SSH config:", error);
      throw Errors.badRequest(
        `Failed to parse SSH config: ${error instanceof Error ? error.message : "Unknown error"}`,
      );
    }
  },
);

/**
 * Routes dispatcher for /api/ssh-configs
 */
export function sshRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  if (path === "/api/ssh-configs" && req.method === "GET") {
    return getSSHConfigs(req);
  }

  // Method not allowed
  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}
