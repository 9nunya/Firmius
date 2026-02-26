import SSEManager from "./manager";
import { Engine } from "@firmius/core";
import EventService from "../services/EventService";
import type { EngineEvent } from "@firmius/shared/types";
import type { BunRequest } from "../types/index";
import type { SSEMessage } from "@firmius/shared/sse";

/**
 * SSE Handlers - Aligned with 1:1 Engine Events.
 * Conversions are removed in favor of direct broadcasting.
 */

export function subscribeToEngineEvents(): void {
  // Subscribe to ALL events from the engine and broadcast them directly.
  // This eliminates the manual conversion bridge and prevents data loss.
  
  const forwardEvent = (event: EngineEvent) => {
    // We broadcast the raw engine event directly as the SSE payload.
    // Our shared/sse/parser.ts is now a 1:1 pass-through.
    SSEManager.broadcast(event.threadId, event as any as SSEMessage);
    EventService.addEvent(event.threadId, event as any as SSEMessage);
  };

  Engine.eventEmitter.on("agent_spawned", forwardEvent);
  Engine.eventEmitter.on("agent_thinking", forwardEvent);
  Engine.eventEmitter.on("agent_content", forwardEvent);
  Engine.eventEmitter.on("agent_provider_request", forwardEvent);
  Engine.eventEmitter.on("tool_call_start", forwardEvent);
  Engine.eventEmitter.on("tool_call_end", forwardEvent);
  Engine.eventEmitter.on("agent_terminated", forwardEvent);
  Engine.eventEmitter.on("agent_file_changed", forwardEvent);
  Engine.eventEmitter.on("user_message", forwardEvent);
  Engine.eventEmitter.on("agent_status", forwardEvent);
  Engine.eventEmitter.on("agent_metrics", forwardEvent);
  Engine.eventEmitter.on("process_output", forwardEvent);
  Engine.eventEmitter.on("process_exit", forwardEvent);
  Engine.eventEmitter.on("agent_provider_error", forwardEvent);
  Engine.eventEmitter.on("agent_turn_complete", forwardEvent);
  Engine.eventEmitter.on("tool_call_preparing", forwardEvent);
  Engine.eventEmitter.on("tool_call_update" as any, forwardEvent);
}

export function handleSSEConnection(
  req: BunRequest,
  res: Response,
  threadId: string,
): void {
  const stream = new ReadableStream({
    start(controller) {
      const encoder = new TextEncoder();

      const clientId = SSEManager.addClient(
        threadId,
        res,
        {
          write(chunk: Uint8Array): Promise<void> {
            controller.enqueue(chunk);
            return Promise.resolve();
          },
          close(): void {
            controller.close();
          },
          abort(reason): void {
            controller.error(reason);
          },
        } as WritableStreamDefaultWriter<Uint8Array>,
        req,
      );

      controller.enqueue(encoder.encode(": connected\n\n"));

      req.signal.addEventListener("abort", () => {
        handleClientDisconnect(threadId, clientId);
        controller.close();
      });
    },
  });

  Object.defineProperty(res, "body", { value: stream, writable: false });
}

export function sendKeepAlive(response: Response): void {
  if (!response.body) return;

  const encoder = new TextEncoder();
  const keepAliveComment = encoder.encode(": keep-alive\n\n");

  const writer = new WritableStream({
    write(_chunk) {},
  }).getWriter();

  writer.write(keepAliveComment).catch(() => {
    writer.close();
  });
}

export function handleClientDisconnect(
  threadId: string,
  clientId: string,
): void {
  SSEManager.removeClient(threadId, clientId);
}
