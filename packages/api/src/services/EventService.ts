import type { SSEMessage } from "@firmius/shared/sse";
import { appendFile, readFile, unlink, mkdir, access, writeFile } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";
import { WriteQueue } from "@firmius/shared";

/**
 * Event subscription callback type
 */
type EventCallback = (event: SSEMessage) => void;

const THREADS_DIR = join(homedir(), ".firmius", "threads");

const PERSISTENT_EVENT_TYPES = [
  "agent_provider_request",
  "agent_provider_error",
  "tool_call_start",
  "tool_call_end",
  "agent_thinking",
  "agent_content",
];

const SHOULD_PERSIST = (type: any): boolean => PERSISTENT_EVENT_TYPES.includes(type);

/**
 * EventService - Event aggregation and SSE broadcast preparation
 */
export class EventService {
  private readonly eventHistory: Map<string, SSEMessage[]> = new Map();
  private readonly subscriptions: Map<string, Set<EventCallback>> = new Map();
  private readonly writeQueues: Map<string, WriteQueue> = new Map();
  private readonly MAX_HISTORY_SIZE = 1000;
  private readonly loadedPersisted: Set<string> = new Set();

  private getThreadDir(threadId: string): string {
    return join(THREADS_DIR, threadId);
  }

  private getEventsFilePath(threadId: string): string {
    return join(this.getThreadDir(threadId), "events.jsonl");
  }

  private getWriteQueue(threadId: string): WriteQueue {
    let queue = this.writeQueues.get(threadId);
    if (!queue) {
      queue = new WriteQueue();
      this.writeQueues.set(threadId, queue);
    }
    return queue;
  }

  private async ensureThreadDir(threadId: string): Promise<void> {
    const dir = this.getThreadDir(threadId);
    try {
      await access(dir);
    } catch {
      await mkdir(dir, { recursive: true });
    }
  }

  async persistEvent(threadId: string, event: SSEMessage): Promise<void> {
    if (!SHOULD_PERSIST(event.type)) {
      return;
    }

    try {
      await this.ensureThreadDir(threadId);
      const filePath = this.getEventsFilePath(threadId);
      const queue = this.getWriteQueue(threadId);
      
      await queue.run(async () => {
        await appendFile(filePath, JSON.stringify(event) + "\n", "utf8");
      });
    } catch (error) {
      console.error(`Failed to persist event for thread ${threadId}:`, error);
    }
  }

  async loadPersistedEvents(threadId: string): Promise<SSEMessage[]> {
    const filePath = this.getEventsFilePath(threadId);
    
    try {
      const content = await readFile(filePath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      
      return lines
        .map(line => {
          try {
            return JSON.parse(line) as SSEMessage;
          } catch {
            return null;
          }
        })
        .filter((e): e is SSEMessage => e !== null);
    } catch {
      return [];
    }
  }

  async removeEventsForTurn(threadId: string, agentId: string, turnCount: number): Promise<void> {
    const filePath = this.getEventsFilePath(threadId);
    
    try {
      const content = await readFile(filePath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      
      const filtered = lines.filter(line => {
        try {
          const event = JSON.parse(line) as any;
          return !(event.agentId === agentId && event.turnCount === turnCount && 
            (event.type === "agent_thinking" || event.type === "agent_content"));
        } catch {
          return true;
        }
      });
      
      if (filtered.length === 0) {
        await unlink(filePath);
      } else {
        const queue = this.getWriteQueue(threadId);
        await queue.run(async () => {
          await writeFile(filePath, filtered.join("\n") + "\n", "utf8");
        });
      }
    } catch {
      // File doesn't exist or other error - ignore
    }
  }

  async clearPersistedEvents(threadId: string): Promise<void> {
    const filePath = this.getEventsFilePath(threadId);
    try {
      await unlink(filePath);
    } catch {
      // File doesn't exist - ignore
    }
  }

  async getEventHistory(threadId: string, since?: Date): Promise<SSEMessage[]> {
    if (!this.loadedPersisted.has(threadId)) {
      this.loadedPersisted.add(threadId);
      const persisted = await this.loadPersistedEvents(threadId);
      if (persisted.length > 0) {
        if (!this.eventHistory.has(threadId)) {
          this.eventHistory.set(threadId, []);
        }
        const history = this.eventHistory.get(threadId)!;
        history.push(...persisted);
      }
    }

    const events = this.eventHistory.get(threadId);

    if (!events) {
      return [];
    }

    if (!since) {
      return [...events];
    }

    const sinceTimestamp = since.getTime();
    return events.filter((event: SSEMessage) => {
      const eventTime = typeof event.timestamp === 'number' ? event.timestamp : new Date(event.timestamp).getTime();
      return eventTime > sinceTimestamp;
    });
  }

  addEvent(threadId: string, event: SSEMessage): void {
    if (!this.eventHistory.has(threadId)) {
      this.eventHistory.set(threadId, []);
    }

    const history = this.eventHistory.get(threadId)!;
    history.push(event);

    if (history.length > this.MAX_HISTORY_SIZE) {
      history.shift();
    }

    this.persistEvent(threadId, event).catch(err => {
      console.error(`Failed to persist event:`, err);
    });

    const callbacks = this.subscriptions.get(threadId);
    if (callbacks) {
      for (const callback of callbacks) {
        try {
          callback(event);
        } catch (error) {
          console.error(`Error in event callback for thread ${threadId}:`, error);
        }
      }
    }
  }

  subscribe(threadId: string, callback: EventCallback): () => void {
    if (!this.subscriptions.has(threadId)) {
      this.subscriptions.set(threadId, new Set());
    }

    const callbacks = this.subscriptions.get(threadId)!;
    callbacks.add(callback);

    return () => {
      callbacks.delete(callback);
      if (callbacks.size === 0) {
        this.subscriptions.delete(threadId);
      }
    };
  }

  unsubscribe(threadId: string, callback: EventCallback): void {
    const callbacks = this.subscriptions.get(threadId);
    if (callbacks) {
      callbacks.delete(callback);
      if (callbacks.size === 0) {
        this.subscriptions.delete(threadId);
      }
    }
  }

  getSubscriberCount(threadId: string): number {
    const callbacks = this.subscriptions.get(threadId);
    return callbacks ? callbacks.size : 0;
  }

  clearHistory(threadId: string): void {
    this.eventHistory.delete(threadId);
    this.subscriptions.delete(threadId);
    this.loadedPersisted.delete(threadId);
    this.writeQueues.delete(threadId);
    this.clearPersistedEvents(threadId).catch(err => {
      console.error(`Failed to clear persisted events:`, err);
    });
  }

  createSSEEvent(
    type: any,
    threadId: string,
    agentId?: string,
    readableName?: string,
    data?: any
  ): SSEMessage {
    return {
      type,
      threadId,
      agentId,
      readableName,
      timestamp: Date.now(),
      ...data
    } as any;
  }

  mapAgentToResponse(engineAgent: any, threadId: string): any {
    return {
      id: engineAgent.id,
      purpose: engineAgent.purpose || "General",
      readableName: engineAgent.readableName || engineAgent.id,
      parentId: engineAgent.parentId,
      isLead: engineAgent.isLead || false,
      turnCount: engineAgent.turnCount ?? 1,
      objective: engineAgent.objective || "",
      subagentIds: engineAgent.subagentIds || [],
      status: engineAgent.status || "idle",
      modelId: engineAgent.modelId,
      threadId
    };
  }
}

export default new EventService();
