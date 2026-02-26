/**
 * SSE Module - Barrel Export
 * Consolidated from: src/api/types/sse.ts, src/tui/lib/sse.ts, src/web/src/lib/sse.ts
 */

export * from "./types";
export * from "./client";
export * from "./parser";

export { SSEClient, sseClient } from "./client";
export type {
  MessageHandler,
  ParsedMessageHandler,
  ErrorHandler,
  StatusHandler,
} from "./client";
