import type { ProviderMessage } from "@firmius/shared/types";
import { type BufferEntry, type EntryPriority } from "./Types";

export class RollingResultBuffer {
  private buffer: BufferEntry[] = [];
  private maxTurns: number;
  private maxTokens: number;
  private currentTurnIndex: number = 0;

  constructor(maxTurns: number = 10, maxTokens: number = 50000) {
    this.maxTurns = maxTurns;
    this.maxTokens = maxTokens;
  }

  setMaxTokens(tokens: number): void {
    this.maxTokens = tokens;
  }

  startNewTurn(): number {
    return ++this.currentTurnIndex;
  }

  push(
    toolCallId: string,
    toolName: string,
    content: any,
    priority: EntryPriority = 'normal'
  ): void {
    const tokenCount = this.estimateTokens(content);
    
    const entry: BufferEntry = {
      turnIndex: this.currentTurnIndex,
      toolCallId,
      toolName,
      content: typeof content === 'string' ? content : JSON.stringify(content ?? ""),
      tokenCount,
      timestamp: Date.now(),
      priority,
    };

    this.buffer.push(entry);
  }

  private estimateTokens(content: any): number {
    // Hardened defense: ensure content is stringified before length check
    const str = typeof content === "string" ? content : JSON.stringify(content ?? "");
    return Math.ceil(str.length / 4);
  }

  evictOldest(): BufferEntry | null {
    const nonCriticalIndex = this.buffer.findIndex(e => e.priority !== 'critical');
    if (nonCriticalIndex === -1) return null;
    
    const entry = this.buffer.splice(nonCriticalIndex, 1)[0];
    return entry ?? null;
  }

  evictByPriority(): BufferEntry | null {
    const priorityOrder: EntryPriority[] = ['low', 'normal', 'high'];
    
    for (const priority of priorityOrder) {
      const index = this.buffer.findIndex(e => e.priority === priority);
      if (index !== -1) {
        const entry = this.buffer.splice(index, 1)[0];
        return entry ?? null;
      }
    }
    return null;
  }

  evictToBudget(targetTokens: number): BufferEntry[] {
    const evicted: BufferEntry[] = [];
    
    while (this.getTokenCount() > targetTokens && this.buffer.length > 0) {
      const entry = this.evictOldest();
      if (!entry) break;
      evicted.push(entry);
    }
    
    return evicted;
  }

  getEntriesForContext(maxTokens?: number): BufferEntry[] {
    const limit = maxTokens ?? this.maxTokens;
    const result: BufferEntry[] = [];
    let total = 0;

    for (let i = this.buffer.length - 1; i >= 0; i--) {
      const entry = this.buffer[i]!;
      if (total + entry.tokenCount <= limit) {
        result.unshift(entry);
        total += entry.tokenCount;
      }
    }

    return result;
  }

  getTokenCount(): number {
    return this.buffer.reduce((sum, e) => sum + e.tokenCount, 0);
  }

  getEntryCount(): number {
    return this.buffer.length;
  }

  getTurnCount(): number {
    const turns = new Set(this.buffer.map(e => e.turnIndex));
    return turns.size;
  }

  clear(): void {
    this.buffer = [];
  }

  getBuffer(): BufferEntry[] {
    return [...this.buffer];
  }

  getLastNTurns(n: number): BufferEntry[] {
    const latestTurn = this.currentTurnIndex;
    const startTurn = Math.max(1, latestTurn - n + 1);
    return this.buffer.filter(e => e.turnIndex >= startTurn);
  }

  toProviderMessages(maxTokens?: number): ProviderMessage[] {
    const entries = this.getEntriesForContext(maxTokens);
    const messages: ProviderMessage[] = [];

    for (const entry of entries) {
      let content = entry.content;
      
      if (content.length > 2000 && entry.priority !== 'critical') {
        content = `[${entry.toolName} result - ${entry.tokenCount} tokens]\n` +
          content.substring(0, 500) + 
          '\n... [truncated, use history tools to read full result]';
      }

      messages.push({
        role: 'tool',
        content,
        tool_call_id: entry.toolCallId,
      });
    }

    return messages;
  }

  getEntriesByTurn(turnIndex: number): BufferEntry[] {
    return this.buffer.filter(e => e.turnIndex === turnIndex);
  }

  removeEntriesByTurn(turnIndex: number): BufferEntry[] {
    const removed = this.buffer.filter(e => e.turnIndex === turnIndex);
    this.buffer = this.buffer.filter(e => e.turnIndex !== turnIndex);
    return removed;
  }

  hasExceededTurnLimit(): boolean {
    return this.getTurnCount() > this.maxTurns;
  }

  trimToTurnLimit(): BufferEntry[] {
    const evicted: BufferEntry[] = [];
    
    while (this.hasExceededTurnLimit()) {
      const oldestTurn = Math.min(...this.buffer.map(e => e.turnIndex));
      const removed = this.removeEntriesByTurn(oldestTurn);
      evicted.push(...removed);
    }
    
    return evicted;
  }
}
