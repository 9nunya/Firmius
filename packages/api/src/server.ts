import ThreadService from "./services/ThreadService";
import { cors, withCORS } from "./middleware/cors";
import { threadRoutes } from "./routes/threads";
import { providerRoutes } from "./routes/providers";
import { agentRoutes } from "./routes/agents";
import { messageRoutes } from "./routes/messages";
import { eventRoutes } from "./routes/events";
import { sshRoutes } from "./routes/ssh";
import { purposeRoutes } from "./routes/purposes";
import { userConfigRoutes } from "./routes/userConfig";
import { fleetRoutes } from "./routes/fleet";
import { getToolDiff } from "./routes/diff";
import { getAgentChanges, getThreadChanges } from "./routes/changes";
import SSEManager from "./sse/manager";
import { subscribeToEngineEvents } from "./sse/handlers";
import { Engine } from "@firmius/core";
import type { BunRequest } from "./types/index";

const PORT = 9174;

interface RouteEntry {
  pattern: RegExp;
  method: string;
  handler: (req: BunRequest) => Response | Promise<Response>;
}

const routes: RouteEntry[] = [
  { pattern: /^\/api\/threads$/, method: "GET", handler: threadRoutes },
  { pattern: /^\/api\/threads$/, method: "POST", handler: threadRoutes },
  { pattern: /^\/api\/threads\/[^/]+$/, method: "GET", handler: threadRoutes },
  {
    pattern: /^\/api\/threads\/[^/]+$/,
    method: "DELETE",
    handler: threadRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/resume$/,
    method: "POST",
    handler: threadRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/interrupt$/,
    method: "POST",
    handler: threadRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/settings$/,
    method: "POST",
    handler: providerRoutes,
  },
  { pattern: /^\/api\/providers$/, method: "GET", handler: providerRoutes },
  {
    pattern: /^\/api\/threads\/[^/]+\/agents$/,
    method: "GET",
    handler: agentRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/history$/,
    method: "GET",
    handler: agentRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/todos$/,
    method: "GET",
    handler: agentRoutes,
  },
  { pattern: /^\/api\/agents\/[^/]+$/, method: "GET", handler: agentRoutes },
  {
    pattern: /^\/api\/threads\/[^/]+\/messages$/,
    method: "GET",
    handler: messageRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/messages$/,
    method: "POST",
    handler: messageRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/messages\/\d+\/edit$/,
    method: "POST",
    handler: messageRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/messages\/\d+\/forget$/,
    method: "POST",
    handler: messageRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/events$/,
    method: "GET",
    handler: eventRoutes,
  },
  { pattern: /^\/api\/ssh-configs$/, method: "GET", handler: sshRoutes },
  { pattern: /^\/api\/purposes$/, method: "GET", handler: purposeRoutes },
  {
    pattern: /^\/api\/user\/config$/,
    method: "GET",
    handler: userConfigRoutes,
  },
  {
    pattern: /^\/api\/user\/config$/,
    method: "POST",
    handler: userConfigRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/status$/,
    method: "GET",
    handler: fleetRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/agents$/,
    method: "GET",
    handler: fleetRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/tasks$/,
    method: "GET",
    handler: fleetRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/mail$/,
    method: "GET",
    handler: fleetRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/mail$/,
    method: "POST",
    handler: fleetRoutes,
  },
  // Model switching endpoint
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/model$/,
    method: "POST",
    handler: agentRoutes,
  },
  // Undo turn endpoint
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/undo-turn$/,
    method: "POST",
    handler: agentRoutes,
  },
  // Diff retrieval endpoint
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/turns\/\d+\/tools\/[^/]+\/diff$/,
    method: "GET",
    handler: getToolDiff,
  },
  // Changes endpoints
  {
    pattern: /^\/api\/threads\/[^/]+\/agents\/[^/]+\/changes$/,
    method: "GET",
    handler: getAgentChanges,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/changes$/,
    method: "GET",
    handler: getThreadChanges,
  },
  // Undo message endpoint
  {
    pattern: /^\/api\/threads\/[^/]+\/messages\/\d+\/undo$/,
    method: "POST",
    handler: messageRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/nudge$/,
    method: "POST",
    handler: fleetRoutes,
  },
  {
    pattern: /^\/api\/threads\/[^/]+\/fleet\/kill$/,
    method: "POST",
    handler: fleetRoutes,
  },
];

async function handleRequest(req: BunRequest): Promise<Response> {
  const url = new URL(req.url);
  const pathname = url.pathname;

  if (pathname === "/sse") {
    const threadId = url.searchParams.get("threadId");

    if (!threadId) {
      return withCORS(
        new Response(
          JSON.stringify({ error: "threadId query parameter is required" }),
          {
            status: 400,
            headers: { "Content-Type": "application/json" },
          },
        ),
        req,
      );
    }

    const stream = new ReadableStream({
      start(controller) {
        const encoder = new TextEncoder();

        const mockResponse = {
          headers: new Headers({ "Content-Type": "text/event-stream" }),
        } as Response;

        let isClosed = false;

        const writer = {
          write: (chunk: Uint8Array): Promise<void> => {
            if (!isClosed) {
              try {
                controller.enqueue(chunk);
                return Promise.resolve();
              } catch (error) {
                isClosed = true;
                return Promise.reject(error);
              }
            }
            return Promise.reject(new Error("Stream closed"));
          },
          close: (): void => {
            if (!isClosed) {
              isClosed = true;
              try {
                controller.close();
              } catch {
                // Ignore close errors
              }
            }
          },
          abort: (reason?: unknown): void => {
            if (!isClosed) {
              isClosed = true;
              try {
                controller.error(reason);
              } catch {
                // Ignore error on already errored controller
              }
            }
          },
        } as WritableStreamDefaultWriter<Uint8Array>;

        // Add client to SSE manager
        SSEManager.addClient(threadId, mockResponse, writer, req);

        // Send initial connection message
        try {
          controller.enqueue(encoder.encode(": connected\n\n"));
        } catch (error) {
          console.error("Failed to send connected message:", error);
        }

        // Handle connection close
        req.signal.addEventListener("abort", () => {
          if (!isClosed) {
            writer.close();
          }
        });

        // Send periodic keep-alive messages
        const sendKeepAlive = () => {
          if (!isClosed) {
            try {
              controller.enqueue(encoder.encode(": keep-alive\n\n"));
            } catch (error) {
              isClosed = true;
              writer.close();
            }
          }
        };

        const interval = setInterval(sendKeepAlive, 15000);

        req.signal.addEventListener("abort", () => {
          clearInterval(interval);
        });
      },
    });

    return withCORS(
      new Response(stream, {
        headers: {
          "Content-Type": "text/event-stream",
          "Cache-Control": "no-cache",
          Connection: "keep-alive",
        },
      }),
      req,
    );
  }

  for (const route of routes) {
    if (route.method === req.method && route.pattern.test(pathname)) {
      try {
        const response = await route.handler(req);
        return withCORS(response, req);
      } catch (error) {
        if (error instanceof Error) {
          return withCORS(
            new Response(JSON.stringify({ error: error.message }), {
              status: 500,
              headers: { "Content-Type": "application/json" },
            }),
            req,
          );
        }
        return withCORS(
          new Response(JSON.stringify({ error: "Internal server error" }), {
            status: 500,
            headers: { "Content-Type": "application/json" },
          }),
          req,
        );
      }
    }
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Not found" }), {
      status: 404,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}

export interface ServerOptions {
  port?: number;
  hostname?: string;
  quiet?: boolean;
  httpLog?: boolean;
  prettyPrint?: boolean;
  staticFileHandler?: (pathname: string) => Promise<Response | null>;
}

export async function createServer(options: ServerOptions = {}): Promise<{ server: ReturnType<typeof Bun.serve>; shutdown: () => Promise<void> }> {
  const port = options.port ?? PORT;
  const hostname = options.hostname ?? "0.0.0.0";
  const quiet = options.quiet ?? false;
  const httpLog = options.httpLog ?? false;
  const prettyPrint = options.prettyPrint ?? false;
  
  if (!quiet) {
    console.log("🔧 Initializing services...");
  }
  
  await Engine.ignite({ prettyPrint });
  
  if (!quiet) {
    console.log("✅ Engine providers and tools loaded");
  }
  
  subscribeToEngineEvents();
  
  if (!quiet) {
    console.log("✅ Engine event subscriptions initialized");
  }

  const fetchHandler = async (req: Request): Promise<Response> => {
    const bunReq = req as BunRequest;
    
    const corsResponse = cors(bunReq, () => new Response(""));
    if (corsResponse.status === 204) {
      return corsResponse;
    }

    const startTime = performance.now();
    const pathname = new URL(req.url).pathname;
    
    if (httpLog) {
      console.log(`[${new Date().toISOString()}] INCOMING: [${bunReq.method}] ${pathname}`);
    }

    // Try static file handler first if provided
    if (options.staticFileHandler && !pathname.startsWith("/api/") && pathname !== "/sse") {
      const staticResponse = await options.staticFileHandler(pathname);
      if (staticResponse) {
        if (httpLog) {
          const duration = Math.round(performance.now() - startTime);
          console.log(`[${new Date().toISOString()}] OUTGOING: [${bunReq.method}] ${pathname} | Status: ${staticResponse.status} | Duration: ${duration}ms`);
        }
        return withCORS(staticResponse, bunReq);
      }
    }

    const response = await handleRequest(bunReq);
    
    if (httpLog) {
      const duration = Math.round(performance.now() - startTime);
      console.log(`[${new Date().toISOString()}] OUTGOING: [${bunReq.method}] ${pathname} | Status: ${response.status} | Duration: ${duration}ms`);
    }

    return withCORS(response, bunReq);
  };

  const server = Bun.serve({
    port,
    hostname,
    fetch: fetchHandler,
    idleTimeout: 60,
  });

  const shutdown = async (): Promise<void> => {
    if (!quiet) {
      console.log("🛑 Shutting down gracefully...");
    }
    
    try {
      const threads = await ThreadService.listThreads();
      if (!quiet) {
        console.log(`💾 Saving checkpoints for ${threads.length} threads...`);
      }
      
      for (const thread of threads) {
        try {
          await ThreadService.saveCheckpoint(thread);
        } catch (error) {
          console.error(`❌ Failed to save checkpoint for thread ${thread.id}:`, error);
        }
      }
      
      if (!quiet) {
        console.log("✅ All checkpoints saved");
        const totalClients = SSEManager.getTotalClientCount();
        console.log(`🔌 Closing ${totalClients} SSE connections...`);
      }
      
      for (const thread of threads) {
        SSEManager.clearThread(thread.id);
      }
      
      if (!quiet) {
        console.log("✅ SSE connections closed");
        console.log("👋 Server shutdown complete");
      }
    } catch (error) {
      console.error("❌ Error during graceful shutdown:", error);
    }
    
    server.stop();
  };

  return { server, shutdown };
}

// Dev server entry point
if (import.meta.main) {
  async function startDevServer(): Promise<void> {
    try {
      const { server, shutdown } = await createServer({ 
        port: PORT, 
        hostname: "0.0.0.0",
        quiet: false,
        httpLog: true,
        prettyPrint: true 
      });

      const shutdownHandler = async (signal: string): Promise<void> => {
        console.log(`\n⚠️  Received ${signal} signal`);
        await shutdown();
        process.exit(0);
      };

      process.on("SIGTERM", () => shutdownHandler("SIGTERM"));
      process.on("SIGINT", () => shutdownHandler("SIGINT"));

      console.log(`🚀 Backend server running on http://0.0.0.0:${server.port}`);
    } catch (error) {
      console.error("❌ Failed to start server:", error);
      process.exit(1);
    }
  }

  startDevServer();
}
