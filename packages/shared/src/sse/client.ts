/**
 * Unified SSE Client
 * Consolidated from: src/tui/lib/sse.ts and src/web/src/lib/sse.ts
 * Supports both browser (EventSource) and Node.js/TUI (fetch streaming)
 */

import type {
  SSEStatus,
  SSEMessage,
  SSEClientConfig,
  SSEConnectionOptions,
} from "./types";
import { parseSSEMessage, parseRawSSEData, parseSSEPart } from "./parser";

export type MessageHandler = (event: SSEMessage) => void;
export type ParsedMessageHandler = (event: ReturnType<typeof parseSSEMessage>) => void;
export type ErrorHandler = (error: Error) => void;
export type StatusHandler = (status: SSEStatus) => void;

function resolveDefaultBaseUrl(): string {
  if (typeof window !== "undefined") {
    const envBackendUrl = typeof process !== "undefined"
      ? process.env?.NEXT_PUBLIC_API_BACKEND_URL
      : undefined;
    if (envBackendUrl) return envBackendUrl;
    return window.location.origin;
  }
  return "http://localhost:9817";
}

const DEFAULT_CONFIG: Required<SSEClientConfig> = {
  baseUrl: "http://localhost:9817",
  reconnect: true,
  reconnectInitialDelay: 1000,
  reconnectMaxDelay: 30000,
  eventBatchWindow: 50,
};

export class SSEClient {
  private config: Required<SSEClientConfig>;
  private eventSource: EventSource | null = null;
  private abortController: AbortController | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectDelay: number;
  private threadId: string | null = null;
  private messageHandlers: Set<MessageHandler> = new Set();
  private parsedMessageHandlers: Set<ParsedMessageHandler> = new Set();
  private errorHandlers: Set<ErrorHandler> = new Set();
  private statusHandlers: Set<StatusHandler> = new Set();
  private currentStatus: SSEStatus = "disconnected";

  private pendingEvents: SSEMessage[] = [];
  private batchTimeoutId: ReturnType<typeof setTimeout> | null = null;

  private useEventSource: boolean;

  constructor(config: SSEClientConfig = {}) {
    this.config = { ...DEFAULT_CONFIG, ...config };
    this.reconnectDelay = this.config.reconnectInitialDelay;
    this.useEventSource = this.detectEventSourceSupport();
  }

  private detectEventSourceSupport(): boolean {
    if (typeof EventSource !== "undefined") {
      return true;
    }
    return false;
  }

  connect(options: string | SSEConnectionOptions): void;
  connect(threadId: string, baseUrl?: string): void;
  connect(optionsOrThreadId: string | SSEConnectionOptions, baseUrlOverride?: string): void {
    let threadId: string;
    let baseUrl: string;

    if (typeof optionsOrThreadId === "object") {
      threadId = optionsOrThreadId.threadId;
      baseUrl = optionsOrThreadId.baseUrl ?? resolveDefaultBaseUrl();
    } else {
      threadId = optionsOrThreadId;
      baseUrl = baseUrlOverride ?? resolveDefaultBaseUrl();
    }

    if (this.threadId === threadId && this.currentStatus === "connected") {
      return;
    }

    this.disconnect();
    this.threadId = threadId;
    this.updateStatus("connecting");

    if (this.useEventSource) {
      this.connectEventSource(threadId, baseUrl);
    } else {
      this.connectFetch(threadId, baseUrl);
    }
  }

  private connectEventSource(threadId: string, baseUrl: string): void {
    try {
      const url = `${baseUrl}/sse?threadId=${threadId}`;
      this.eventSource = new EventSource(url);

      this.eventSource.onopen = () => {
        this.reconnectDelay = this.config.reconnectInitialDelay;
        this.updateStatus("connected");
      };

      this.eventSource.onmessage = (event: MessageEvent) => {
        this.handleMessage(event.data);
      };

      this.eventSource.onerror = () => {
        this.handleError(new Error("SSE connection error"));
      };
    } catch {
      this.handleError(new Error("Failed to create EventSource"));
    }
  }

  private async connectFetch(threadId: string, baseUrl: string): Promise<void> {
    this.abortController = new AbortController();
    const url = `${baseUrl}/sse?threadId=${threadId}`;

    try {
      const response = await fetch(url, {
        signal: this.abortController.signal,
      });

      if (!response.ok) {
        throw new Error(`SSE connection failed: ${response.statusText}`);
      }

      if (!response.body) {
        throw new Error("SSE response body is null");
      }

      this.reconnectDelay = this.config.reconnectInitialDelay;
      this.updateStatus("connected");

      const reader = response.body.getReader();
      const decoder = new TextDecoder();
      let buffer = "";

      while (true) {
        const { done, value } = await reader.read();
        if (done) break;

        buffer += decoder.decode(value, { stream: true });
        const parts = buffer.split("\n\n");
        buffer = parts.pop() || "";

        for (const part of parts) {
          if (part.trim()) {
            const data = parseSSEPart(part);
            if (data) {
              this.handleMessage(data);
            }
          }
        }
      }

      this.handleError(new Error("SSE stream ended"));
    } catch (error) {
      if (error instanceof Error && error.name === "AbortError") {
        return;
      }
      this.handleError(error instanceof Error ? error : new Error(String(error)));
    }
  }

  private handleMessage(data: string): void {
    const message = parseRawSSEData(data);
    if (!message) {
      console.warn("Failed to parse SSE message:", data);
      return;
    }

    if (!this.validateMessage(message)) {
      console.warn("Invalid SSE message format:", message);
      return;
    }

    if (this.config.eventBatchWindow > 0) {
      this.queueEvent(message);
    } else {
      this.emitMessage(message);
    }
  }

  private queueEvent(event: SSEMessage): void {
    this.pendingEvents.push(event);

    if (!this.batchTimeoutId) {
      this.batchTimeoutId = setTimeout(() => {
        this.flushPendingEvents();
      }, this.config.eventBatchWindow);
    }
  }

  private flushPendingEvents(): void {
    if (this.batchTimeoutId) {
      clearTimeout(this.batchTimeoutId);
      this.batchTimeoutId = null;
    }

    if (this.pendingEvents.length === 0) return;

    const eventsToProcess = [...this.pendingEvents];
    this.pendingEvents = [];

    for (const event of eventsToProcess) {
      this.emitMessage(event);
    }
  }

  private emitMessage(message: SSEMessage): void {
    this.messageHandlers.forEach((handler) => handler(message));

    const parsed = parseSSEMessage(message);
    if (parsed) {
      this.parsedMessageHandlers.forEach((handler) => handler(parsed));
    }
  }

  private validateMessage(message: SSEMessage): boolean {
    return (
      !!message &&
      typeof message === "object" &&
      typeof message.type === "string" &&
      typeof message.threadId === "string"
    );
  }

  disconnect(): void {
    if (this.batchTimeoutId) {
      clearTimeout(this.batchTimeoutId);
      this.batchTimeoutId = null;
    }

    this.flushPendingEvents();

    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }

    if (this.eventSource) {
      this.eventSource.close();
      this.eventSource = null;
    }

    if (this.abortController) {
      this.abortController.abort();
      this.abortController = null;
    }

    this.threadId = null;
    this.updateStatus("disconnected");
  }

  onMessage(callback: MessageHandler): () => void {
    this.messageHandlers.add(callback);
    return () => this.messageHandlers.delete(callback);
  }

  onParsedMessage(callback: ParsedMessageHandler): () => void {
    this.parsedMessageHandlers.add(callback);
    return () => this.parsedMessageHandlers.delete(callback);
  }

  onError(callback: ErrorHandler): () => void {
    this.errorHandlers.add(callback);
    return () => this.errorHandlers.delete(callback);
  }

  onStatusChange(callback: StatusHandler): () => void {
    this.statusHandlers.add(callback);
    callback(this.currentStatus);
    return () => this.statusHandlers.delete(callback);
  }

  getStatus(): SSEStatus {
    return this.currentStatus;
  }

  getThreadId(): string | null {
    return this.threadId;
  }

  private handleError(error: Error): void {
    this.updateStatus("error");
    this.errorHandlers.forEach((handler) => handler(error));

    if (this.eventSource) {
      this.eventSource.close();
      this.eventSource = null;
    }

    if (this.config.reconnect && this.threadId) {
      this.scheduleReconnect();
    }
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
    }

    this.reconnectTimer = setTimeout(() => {
      if (this.threadId) {
        this.connect(this.threadId);
      }
    }, this.reconnectDelay);

    this.reconnectDelay = Math.min(
      this.reconnectDelay * 2,
      this.config.reconnectMaxDelay
    );
  }

  private updateStatus(status: SSEStatus): void {
    this.currentStatus = status;
    this.statusHandlers.forEach((handler) => handler(status));
  }
}

export const sseClient = new SSEClient();
export default sseClient;
