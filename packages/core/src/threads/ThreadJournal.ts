import { appendFile, readFile, writeFile } from "node:fs/promises";
import { WriteQueue } from "@firmius/shared";
import type { AgentTurn, AgentConversationalMessage, IAgent } from "@firmius/shared";
import { AgentWorkType } from "@firmius/shared";

/**
 * JournalEntry represents a single entry in a journal.
 */
export interface JournalEntry {
  sequence: number;
  agentId: string;
  timestamp: number;
  type: "turn" | "message" | "forgotten" | "message_edited";
  payload: any;
}

/**
 * AgentJournal - per-agent journal for turns and messages.
 */
export class AgentJournal {
  private readonly journalPath: string;
  private readonly writeQueue: WriteQueue;
  private sequenceCounter: number;
  private readonly defaultAgentId: string;

  constructor(journalPath: string, initialSequence: number = 0, defaultAgentId: string = "") {
    this.journalPath = journalPath;
    this.sequenceCounter = initialSequence;
    this.defaultAgentId = defaultAgentId;
    this.writeQueue = new WriteQueue();
  }

  public async ensureInitialized(): Promise<void> {
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, "", "utf8");
    });
  }

  public getSequenceCounter(): number {
    return this.sequenceCounter;
  }

  public setSequenceCounter(sequence: number): void {
    this.sequenceCounter = sequence;
  }

  public getJournalPath(): string {
    return this.journalPath;
  }

  public async recordTurn(agentId: string, turn: AgentTurn, forcedSequence?: number): Promise<number> {
    const sequence = forcedSequence !== undefined ? forcedSequence : this.sequenceCounter++;
    this.sequenceCounter = Math.max(this.sequenceCounter, sequence + 1);
    const entry: JournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "turn",
      payload: turn
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return sequence;
  }

  public async recordMessage(agentId: string, message: AgentConversationalMessage, forcedSequence?: number): Promise<number> {
    const sequence = forcedSequence !== undefined ? forcedSequence : this.sequenceCounter++;
    this.sequenceCounter = Math.max(this.sequenceCounter, sequence + 1);
    const entry: JournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "message",
      payload: message
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return sequence;
  }

  public async forgetEntry(sequence: number): Promise<void> {
    const isAlreadyForgotten = await this.isEntryForgotten(sequence);
    if (isAlreadyForgotten) {
      throw new Error("Entry is already forgotten");
    }
    const entry: JournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: "",
      timestamp: Date.now(),
      type: "forgotten",
      payload: { targetSequence: sequence, forgottenAt: Date.now() }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
  }

  public async unforgetEntry(sequence: number): Promise<void> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      const filtered = lines.filter(line => {
        try {
          const entry = JSON.parse(line) as JournalEntry;
          return !(entry.type === "forgotten" && (entry.payload as any).targetSequence === sequence);
        } catch {
          return true;
        }
      });
      await writeFile(this.journalPath, filtered.join("\n") + "\n", "utf8");
    } catch {
      // Journal doesn't exist
    }
  }

  public async editUserMessage(sequence: number, newContent: string): Promise<void> {
    let targetEntry: JournalEntry | null = null;
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      for (const line of lines) {
        const entry = JSON.parse(line) as JournalEntry;
        if (entry.sequence === sequence) {
          targetEntry = entry;
          break;
        }
      }
    } catch {
      throw new Error("Journal not found");
    }
    if (!targetEntry) {
      throw new Error("Entry not found");
    }
    if (targetEntry.type !== "message" || !targetEntry.payload.isUser) {
      throw new Error("Can only edit user messages");
    }
    const editEntry: JournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: targetEntry.agentId,
      timestamp: Date.now(),
      type: "message_edited",
      payload: { targetSequence: sequence, newContent, editedAt: Date.now() }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(editEntry) + "\n", "utf8");
    });
  }

  public async getLastUserMessage(): Promise<{ sequence: number; content: string } | null> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      let lastUserMsg: { sequence: number; content: string } | null = null;
      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i];
        if (!line) continue;
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.type === "message" && entry.payload.isUser) {
            lastUserMsg = { sequence: entry.sequence, content: entry.payload.content };
            break;
          }
        } catch { }
      }
      return lastUserMsg;
    } catch {
      return null;
    }
  }

  public async getLastAgentTurn(agentId: string): Promise<number | null> {
    if (!agentId) return null;
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      let lastTurnSeq: number | null = null;
      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i];
        if (!line) continue;
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.type === "turn" && entry.agentId === agentId) {
            lastTurnSeq = entry.sequence;
            break;
          }
        } catch { }
      }
      return lastTurnSeq;
    } catch {
      return null;
    }
  }

  public async readAllEntries(): Promise<JournalEntry[]> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      return lines.map(line => {
        try {
          return JSON.parse(line) as JournalEntry;
        } catch {
          return null;
        }
      }).filter((e): e is JournalEntry => e !== null);
    } catch {
      return [];
    }
  }

  public async forgetEventsAfterSequence(sequence: number): Promise<void> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      const entries = lines.map((line, index) => {
        try {
          return { index, entry: JSON.parse(line) as JournalEntry };
        } catch {
          return null;
        }
      }).filter(e => e !== null);
      for (const item of entries) {
        if (item && item.entry && item.entry.sequence >= sequence) {
          await this.forgetEntry(item.entry.sequence);
        }
      }
    } catch { }
  }

  public async forgetLastTurn(): Promise<void> {
    const lastTurnSeq = await this.getLastAgentTurn(this.defaultAgentId);
    if (lastTurnSeq === null) {
      throw new Error("No agent turn to forget");
    }
    await this.forgetEntry(lastTurnSeq);
  }

  public async replayFrom(
    startSequence: number,
    agentFactory: any,
    agents: Map<string, IAgent>
  ): Promise<void> {
    const forgottenSequences = new Set<number>();
    const editedMessages = new Map<number, string>();
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      // First pass: collect forgotten and edited
      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.sequence >= startSequence) {
            if (entry.type === "forgotten") {
              forgottenSequences.add((entry.payload as any).targetSequence);
            } else if (entry.type === "message_edited") {
              editedMessages.set((entry.payload as any).targetSequence, (entry.payload as any).newContent);
            }
          }
        } catch { }
      }
      // Second pass: replay
      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.sequence < startSequence) continue;
          if (entry.type === "forgotten" || entry.type === "message_edited") continue;
          if (forgottenSequences.has(entry.sequence)) continue;
          let targetAgent = agents.get(entry.agentId);
          if (!targetAgent && agentFactory?.agents?.get) {
            targetAgent = agentFactory.agents.get(entry.agentId) as IAgent | undefined;
          }
          if (targetAgent?.context) {
            const ctx = targetAgent.context;
            if (entry.type === "turn") {
              const turn = entry.payload as AgentTurn;
              if (ctx.historyData.history.type === AgentWorkType.Goal) {
                if (!ctx.historyData.history.workflow) {
                  ctx.historyData.history.workflow = { turns: [], timestamp: Date.now(), completed: false, finalMessage: null };
                }
                ctx.historyData.history.workflow.turns.push(turn);
              } else if (ctx.historyData.history.type === AgentWorkType.Conversational) {
                if (!ctx.historyData.history.conversation) {
                  ctx.historyData.history.conversation = { history: [] };
                }
                if (turn.toolCalls && turn.toolCalls.length > 0) {
                  ctx.historyData.history.conversation.history.push({
                    turns: [turn],
                    timestamp: turn.timestamp,
                    completed: true,
                    finalMessage: typeof turn.content === 'string' ? turn.content : (Array.isArray(turn.content) ? turn.content.map((p: any) => p.text || '').join('') : null)
                  });
                } else {
                  ctx.historyData.history.conversation.history.push({
                    isUser: false,
                    content: turn.content || "",
                    timestamp: turn.timestamp,
                    tokens: turn.tokens,
                    reasoning: turn.reasoning
                  });
                }
              }
            } else if (entry.type === "message") {
              let msg = entry.payload as AgentConversationalMessage;
              if (editedMessages.has(entry.sequence)) {
                msg = { ...msg, content: editedMessages.get(entry.sequence)! };
              }
              if (ctx.historyData.history.type === AgentWorkType.Conversational) {
                if (!ctx.historyData.history.conversation) {
                  ctx.historyData.history.conversation = { history: [] };
                }
                ctx.historyData.history.conversation.history.push(msg);
              }
            }
          }
        } catch { }
      }
    } catch {
      // Journal doesn't exist or other error
    }
  }

  private async isEntryForgotten(sequence: number): Promise<boolean> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      for (const line of lines) {
        const entry = JSON.parse(line) as JournalEntry;
        if (entry.type === "forgotten" && (entry.payload as any).targetSequence === sequence) {
          return true;
        }
      }
    } catch {
      // Journal doesn't exist yet
    }
    return false;
  }

  public async flush(): Promise<void> {
    await this.writeQueue.flush();
  }
}

/**
 * ThreadMetadataJournal - thread-level journal for user messages and system events.
 */
export class ThreadMetadataJournal {
  private readonly journalPath: string;
  private readonly writeQueue: WriteQueue;
  private sequenceCounter: number;

  constructor(journalPath: string, initialSequence: number = 0) {
    this.journalPath = journalPath;
    this.sequenceCounter = initialSequence;
    this.writeQueue = new WriteQueue();
  }

  public async ensureInitialized(): Promise<void> {
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, "", "utf8");
    });
  }

  public getSequenceCounter(): number {
    return this.sequenceCounter;
  }

  public setSequenceCounter(sequence: number): void {
    this.sequenceCounter = sequence;
  }

  public getJournalPath(): string {
    return this.journalPath;
  }

  public async recordUserMessage(agentId: string, message: AgentConversationalMessage, forcedSequence?: number): Promise<number> {
    const sequence = forcedSequence !== undefined ? forcedSequence : this.sequenceCounter++;
    this.sequenceCounter = Math.max(this.sequenceCounter, sequence + 1);
    const entry: JournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "message",
      payload: message
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return sequence;
  }

  public async recordAgentSpawned(agentId: string, purpose: string, parentId: string, forcedSequence?: number): Promise<number> {
    const sequence = forcedSequence !== undefined ? forcedSequence : this.sequenceCounter++;
    this.sequenceCounter = Math.max(this.sequenceCounter, sequence + 1);
    const entry: JournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "message",
      payload: {
        isUser: false,
        isSystem: true,
        content: `Agent spawned: ${purpose} (${agentId})`,
        parentId,
        purpose
      }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return sequence;
  }

  public async recordAgentTerminated(agentId: string, reason: string, forcedSequence?: number): Promise<number> {
    const sequence = forcedSequence !== undefined ? forcedSequence : this.sequenceCounter++;
    this.sequenceCounter = Math.max(this.sequenceCounter, sequence + 1);
    const entry: JournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "message",
      payload: {
        isUser: false,
        isSystem: true,
        content: `Agent terminated: ${reason}`,
        reason
      }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return sequence;
  }

  public async recordMessageEdited(sequence: number, newContent: string): Promise<number> {
    const entry: JournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: "",
      timestamp: Date.now(),
      type: "message_edited",
      payload: { targetSequence: sequence, newContent, editedAt: Date.now() }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
    return entry.sequence;
  }

  public async forgetEntry(sequence: number): Promise<void> {
    const isAlreadyForgotten = await this.isEntryForgotten(sequence);
    if (isAlreadyForgotten) {
      throw new Error("Entry is already forgotten");
    }
    const entry: JournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: "",
      timestamp: Date.now(),
      type: "forgotten",
      payload: { targetSequence: sequence, forgottenAt: Date.now() }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(entry) + "\n", "utf8");
    });
  }

  public async unforgetEntry(sequence: number): Promise<void> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      const filtered = lines.filter(line => {
        try {
          const entry = JSON.parse(line) as JournalEntry;
          return !(entry.type === "forgotten" && (entry.payload as any).targetSequence === sequence);
        } catch {
          return true;
        }
      });
      await writeFile(this.journalPath, filtered.join("\n") + "\n", "utf8");
    } catch {
      // Journal doesn't exist
    }
  }

  public async getLastUserMessage(): Promise<{ sequence: number; content: string } | null> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      let lastUserMsg: { sequence: number; content: string } | null = null;
      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i];
        if (!line) continue;
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.type === "message" && entry.payload.isUser) {
            lastUserMsg = { sequence: entry.sequence, content: entry.payload.content };
            break;
          }
        } catch { }
      }
      return lastUserMsg;
    } catch {
      return null;
    }
  }

  public async readAllEntries(): Promise<JournalEntry[]> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      return lines
        .map(line => {
          try {
            return JSON.parse(line) as JournalEntry;
          } catch {
            return null;
          }
        })
        .filter((entry): entry is JournalEntry => entry !== null);
    } catch {
      return [];
    }
  }

  public async editUserMessage(sequence: number, newContent: string): Promise<void> {
    let targetEntry: JournalEntry | null = null;
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      for (const line of lines) {
        const entry = JSON.parse(line) as JournalEntry;
        if (entry.sequence === sequence) {
          targetEntry = entry;
          break;
        }
      }
    } catch {
      throw new Error("Journal not found");
    }
    if (!targetEntry) {
      throw new Error("Entry not found");
    }
    if (targetEntry.type !== "message" || !targetEntry.payload.isUser) {
      throw new Error("Can only edit user messages");
    }
    const editEntry: JournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: targetEntry.agentId,
      timestamp: Date.now(),
      type: "message_edited",
      payload: { targetSequence: sequence, newContent, editedAt: Date.now() }
    };
    await this.writeQueue.run(async () => {
      await appendFile(this.journalPath, JSON.stringify(editEntry) + "\n", "utf8");
    });
  }

  public async forgetEventsAfterSequence(sequence: number): Promise<void> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      const entries = lines.map((line, index) => {
        try {
          return { index, entry: JSON.parse(line) as JournalEntry };
        } catch {
          return null;
        }
      }).filter(e => e !== null);
      for (const item of entries) {
        if (item && item.entry && item.entry.sequence >= sequence) {
          await this.forgetEntry(item.entry.sequence);
        }
      }
    } catch { }
  }

  public async forgetLastTurn(): Promise<void> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      let lastUserSeq: number | null = null;
      for (let i = lines.length - 1; i >= 0; i--) {
        const line = lines[i];
        if (!line) continue;
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.type === "message" && entry.payload.isUser) {
            lastUserSeq = entry.sequence;
            break;
          }
        } catch { }
      }
      if (lastUserSeq !== null) {
        await this.forgetEntry(lastUserSeq);
      }
    } catch { }
  }

  private async isEntryForgotten(sequence: number): Promise<boolean> {
    try {
      const content = await readFile(this.journalPath, "utf8");
      const lines = content.split("\n").filter(l => l.trim());
      for (const line of lines) {
        const entry = JSON.parse(line) as JournalEntry;
        if (entry.type === "forgotten" && (entry.payload as any).targetSequence === sequence) {
          return true;
        }
      }
    } catch {
      // Journal doesn't exist yet
    }
    return false;
  }

  public async flush(): Promise<void> {
    await this.writeQueue.flush();
  }
}
