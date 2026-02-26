import { randomUUID } from "node:crypto";
import type { IThread } from "@firmius/shared";
import { AgentWorkType } from "@firmius/shared";
import type { AgentPurpose, IAgent, AgentTurn, AgentConversationalMessage } from "@firmius/shared";
import type { HostConfig } from "@firmius/shared";
import { ThreadManager } from "./ThreadManager";
import { AgentRegistry } from "@firmius/core/registry";
import { Engine } from "@firmius/core";
import { HostFactory } from "@firmius/core";
import { Coordinator } from "@firmius/core";
type AgentStates = Record<string, unknown>;

/**
 * Represents a single in-memory journal entry.
 */
interface InMemoryJournalEntry {
  sequence: number;
  agentId: string;
  timestamp: number;
  type: "turn" | "message" | "forgotten" | "message_edited";
  payload: any;
}

/**
 * In-memory journal that provides journal operations without disk persistence.
 * This class mirrors the interface of ThreadJournal but stores entries in memory.
 */
class InMemoryThreadJournal {
  private entries: InMemoryJournalEntry[] = [];
  private sequenceCounter: number = 0;
  private readonly defaultAgentId: string;

  /**
   * Creates a new InMemoryThreadJournal instance.
   *
   * @param defaultAgentId - The default agent ID for operations like forgetLastTurn
   */
  constructor(defaultAgentId: string = "") {
    this.defaultAgentId = defaultAgentId;
  }

  /**
   * Gets the current sequence counter value.
   */
  public getSequenceCounter(): number {
    return this.sequenceCounter;
  }

  /**
   * Sets the sequence counter to a specific value.
   */
  public setSequenceCounter(sequence: number): void {
    this.sequenceCounter = sequence;
  }

  /**
   * Records an agent turn to the journal.
   */
  public async recordTurn(agentId: string, turn: AgentTurn): Promise<number> {
    const sequence = this.sequenceCounter++;
    const entry: InMemoryJournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "turn",
      payload: turn
    };
    this.entries.push(entry);
    return sequence;
  }

  /**
   * Records a conversational message to the journal.
   */
  public async recordMessage(agentId: string, message: AgentConversationalMessage): Promise<number> {
    const sequence = this.sequenceCounter++;
    const entry: InMemoryJournalEntry = {
      sequence,
      agentId,
      timestamp: Date.now(),
      type: "message",
      payload: message
    };
    this.entries.push(entry);
    return sequence;
  }

  /**
   * Marks an entry as forgotten by adding a forgotten marker.
   */
  public async forgetEntry(sequence: number): Promise<void> {
    const isAlreadyForgotten = this.entries.some(
      e => e.type === "forgotten" && e.payload.targetSequence === sequence
    );
    if (isAlreadyForgotten) {
      throw new Error("Entry is already forgotten");
    }

    const entry: InMemoryJournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: "",
      timestamp: Date.now(),
      type: "forgotten",
      payload: { targetSequence: sequence, forgottenAt: Date.now() }
    };
    this.entries.push(entry);
  }

  /**
   * Removes the forgotten marker from an entry.
   */
  public async unforgetEntry(sequence: number): Promise<void> {
    this.entries = this.entries.filter(
      entry => !(entry.type === "forgotten" && entry.payload.targetSequence === sequence)
    );
  }

  /**
   * Edits a user message content.
   */
  public async editUserMessage(sequence: number, newContent: string): Promise<void> {
    const targetEntry = this.entries.find(e => e.sequence === sequence);
    if (!targetEntry) {
      throw new Error("Entry not found");
    }

    if (targetEntry.type !== "message" || !targetEntry.payload.isUser) {
      throw new Error("Can only edit user messages");
    }

    const editEntry: InMemoryJournalEntry = {
      sequence: this.sequenceCounter++,
      agentId: targetEntry.agentId,
      timestamp: Date.now(),
      type: "message_edited",
      payload: { targetSequence: sequence, newContent, editedAt: Date.now() }
    };
    this.entries.push(editEntry);
  }

  /**
   * Gets the last user message from the journal.
   */
  public async getLastUserMessage(): Promise<{ sequence: number; content: string } | null> {
    for (let i = this.entries.length - 1; i >= 0; i--) {
      const entry = this.entries[i];
      if (!entry) continue;
      if (entry.type === "message" && entry.payload.isUser) {
        return { sequence: entry.sequence, content: entry.payload.content };
      }
    }
    return null;
  }

  /**
   * Gets the sequence number of the last turn for a specific agent.
   */
  public async getLastAgentTurn(agentId: string): Promise<number | null> {
    if (!agentId) return null;

    for (let i = this.entries.length - 1; i >= 0; i--) {
      const entry = this.entries[i];
      if (!entry) continue;
      if (entry.type === "turn" && entry.agentId === agentId) {
        return entry.sequence;
      }
    }
    return null;
  }

  /**
   * Forgets all events after a specific sequence number.
   */
  public async forgetEventsAfterSequence(sequence: number): Promise<void> {
    const entriesToForget = this.entries.filter(e => e.sequence > sequence);
    for (const entry of entriesToForget) {
      await this.forgetEntry(entry.sequence);
    }
  }

  /**
   * Forgets the last turn in the journal.
   */
  public async forgetLastTurn(): Promise<void> {
    const lastTurnSeq = await this.getLastAgentTurn(this.defaultAgentId);
    if (lastTurnSeq === null) {
      throw new Error("No agent turn to forget");
    }
    await this.forgetEntry(lastTurnSeq);
  }

  /**
   * Replays journal entries from a starting sequence, reconstructing agent state.
   */
  public async replayFrom(
    startSequence: number,
    agentFactory: any,
    agents: Map<string, IAgent>
  ): Promise<void> {
    const forgottenSequences = new Set<number>();
    const editedMessages = new Map<number, string>();

    // First pass: collect forgotten and edited entries
    for (const entry of this.entries) {
      if (entry.sequence >= startSequence) {
        if (entry.type === "forgotten") {
          forgottenSequences.add(entry.payload.targetSequence);
        } else if (entry.type === "message_edited") {
          editedMessages.set(entry.payload.targetSequence, entry.payload.newContent);
        }
      }
    }

    // Second pass: replay non-forgotten entries
    for (const entry of this.entries) {
      if (entry.sequence < startSequence) continue;
      if (entry.type === "forgotten" || entry.type === "message_edited") continue;
      if (forgottenSequences.has(entry.sequence)) continue;

      let targetAgent = agents.get(entry.agentId);
      if (!targetAgent && agentFactory?.agents?.get) {
        targetAgent = agentFactory.agents.get(entry.agentId) as IAgent | undefined;
      }

      if (targetAgent) {
        const ctx = targetAgent.context!;
        if (entry.type === "turn") {
          const turn = entry.payload as AgentTurn;

          if (ctx.historyData.history.type === AgentWorkType.Goal) {
            if (!ctx.historyData.history.workflow) {
              ctx.historyData.history.workflow = {
                turns: [],
                timestamp: Date.now(),
                completed: false,
                finalMessage: null
              };
            }
            ctx.historyData.history.workflow.turns.push(turn);
          } else if (ctx.historyData.history.type === AgentWorkType.Conversational) {
            if (!ctx.historyData.history.conversation) {
              ctx.historyData.history.conversation = { history: [] };
            }

            if (turn.toolCalls && turn.toolCalls.length > 0) {
              ctx.historyData.history.conversation!.history.push({
                turns: [turn],
                timestamp: turn.timestamp,
                completed: true,
                finalMessage: typeof turn.content === 'string' ? turn.content : (Array.isArray(turn.content) ? turn.content.map((p: any) => p.text || '').join('') : null)
              });
            } else {
              ctx.historyData.history.conversation!.history.push({
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
    }
  }

  /**
   * Reads all journal entries.
   */
  public async readAllEntries(): Promise<InMemoryJournalEntry[]> {
    return [...this.entries];
  }

  /**
   * Flushes pending writes (no-op for in-memory).
   */
  public async flush(): Promise<void> {
    // No-op for in-memory journal
  }

  /**
   * Gets the journal path (returns empty string for in-memory).
   */
  public getJournalPath(): string {
    return "";
  }
}

/**
 * In-memory thread implementation for testing and temporary sessions.
 *
 * This class provides a thread-like experience without disk persistence.
 * It uses ThreadJournal for journal operations (in-memory variant),
 * ThreadManager for lifecycle management, and AgentRegistry for agent tracking.
 *
 * Useful for:
 * - Testing scenarios where disk I/O is not desired
 * - Temporary threads that do not need to be restored
 * - Sessions where data should only exist in memory
 */
export class InMemoryThread implements IThread {
  public readonly id: string;
  public readonly hostConfig: HostConfig;
  public readonly rootCwd: string;
  public readonly leadAgent: IAgent;
  public readonly coordinator: Coordinator;
  public title: string | undefined;
  public interrupted: boolean = false;
  public agentStates: AgentStates = {};
  public checkpointedAt: number = Date.now();

  private readonly agentRegistry: AgentRegistry;
  private readonly threadManager: ThreadManager;
  private readonly journal: InMemoryThreadJournal;
  private isDisposed: boolean = false;

  /**
   * Private constructor - use static create() method instead.
   */
  private constructor(
    id: string,
    hostConfig: HostConfig,
    rootCwd: string,
    leadAgent: IAgent,
    coordinator: Coordinator,
    agentRegistry: AgentRegistry,
    threadManager: ThreadManager,
    journal: InMemoryThreadJournal,
    title?: string
  ) {
    this.id = id;
    this.hostConfig = hostConfig;
    this.rootCwd = rootCwd;
    this.leadAgent = leadAgent;
    this.coordinator = coordinator;
    this.agentRegistry = agentRegistry;
    this.threadManager = threadManager;
    this.journal = journal;
    this.title = title;
  }

  /**
   * Creates a new in-memory thread.
   *
   * @param options - Thread creation options
   * @param options.hostConfig - The host configuration
   * @param options.rootCwd - The root working directory
   * @param options.purpose - The agent purpose
   * @param options.objective - The agent objective
   * @param options.workType - The work type (defaults to Conversational)
   * @param options.generationOptions - Generation options for the agent
   * @param options.parentId - Optional parent agent ID for subagents
   * @returns A new InMemoryThread instance
   */
  static async create(options: {
    hostConfig: HostConfig;
    rootCwd: string;
    purpose: AgentPurpose;
    objective: string;
    workType?: AgentWorkType;
    generationOptions?: { providerId: string; modelId: string; reasoningEffort?: string; maxTokens?: number };
    parentId?: string;
  }): Promise<InMemoryThread> {
    const threadId = randomUUID();

    // Create host
    const host = await HostFactory.create(options.hostConfig);
    await host.init();

    // Create agent registry
    const agentRegistry = new AgentRegistry(threadId);

    // Create lead agent via Engine's agent factory
    const agentFactory = Engine.agentFactory;
    const leadAgent = await agentFactory.summon({
      purpose: options.purpose,
      objective: options.objective,
      cwd: options.rootCwd,
      host,
      workType: AgentWorkType.Conversational,
      generationOptions: options.generationOptions as any,
      parentId: options.parentId,
      threadId,
      onTurn: undefined
    });

    // Register lead agent in the registry
    agentRegistry.set(leadAgent.id, leadAgent);

    // Create thread manager for lifecycle
    const threadManager = new ThreadManager(threadId, new Set<IAgent>([leadAgent]), host);

    // Create in-memory journal
    const journal = new InMemoryThreadJournal(leadAgent.id);

    // Extract title from objective
    const objective = options.objective ?? "";
    const title = (objective.split("\n")[0] || "New Thread").trim().substring(0, 60);

    const coordinator = await Coordinator.create(host, options.rootCwd, threadId);
    const thread = new InMemoryThread(
      threadId,
      options.hostConfig,
      options.rootCwd,
      leadAgent,
      coordinator,
      agentRegistry,
      threadManager,
      journal,
      title
    );

    // Set up onTurn callbacks for lead agent and subagents
    const setOnTurn = (agent: IAgent) => {
      agent.context!.io.onTurn = (turn, agentId) => thread.recordTurn(agentId, turn);
      for (const subId of agent.context!.identity.subagentIds) {
        const sub = agentFactory.agents.get(subId) as IAgent | undefined;
        if (sub) setOnTurn(sub);
      }
    };
    setOnTurn(leadAgent);

    // Listen for agent spawn events
    const onAgentSpawned = (event: any) => {
      const parentAgent = agentFactory.agents.get(event.parentId) as IAgent | undefined;
      if (event.parentId && parentAgent && thread.agentRegistry.has(parentAgent.id)) {
        const newAgent = agentFactory.agents.get(event.agentId) as IAgent | undefined;
        if (newAgent) {
          thread.agentRegistry.set(newAgent.id, newAgent);
          (thread.threadManager as any).agents.add(newAgent);
          setOnTurn(newAgent);
        }
      }
    };
    Engine.eventEmitter.on("agent_spawned", onAgentSpawned);
    (thread as any)._spawnListener = onAgentSpawned;

    return thread;
  }

  /**
   * Gets all subagents in the thread.
   */
  getSubagents(): IAgent[] {
    return this.agentRegistry.getAll().filter(a => a !== this.leadAgent);
  }

  /**
   * Gets all agent IDs in the thread.
   */
  getAllAgentIds(): string[] {
    return this.agentRegistry.getAll().map(a => a.id);
  }

  /**
   * Gets the total tokens used by the lead agent.
   */
  public get tokensUsed(): number {
    return this.leadAgent.context!.state.metrics.totalTokens;
  }

  /**
   * Interrupts all agents in the thread.
   */
  async interrupt(): Promise<void> {
    this.interrupted = true;
    await this.threadManager.interrupt();
    this.interrupted = false;
  }

  /**
   * Cancels the current request.
   */
  async cancelRequest(): Promise<void> {
    await this.threadManager.cancelRequest();
  }

  /**
   * Performs a checkpoint (no-op for in-memory thread).
   */
  async checkpoint(_path?: string): Promise<void> {
    // No-op for in-memory thread - no persistence
  }

  /**
   * Creates a new branch starting from a specific sequence number.
   * For in-memory threads, this is a simplified operation that just returns a new branch ID.
   *
   * @param _sequence - The sequence number to branch from
   * @returns A new branch ID
   */
  async branch(_sequence: number): Promise<string> {
    return randomUUID().substring(0, 8);
  }

  /**
   * Records an agent turn to the journal.
   */
  async recordTurn(agentId: string, turn: AgentTurn): Promise<number> {
    return this.journal.recordTurn(agentId, turn);
  }

  /**
   * Records a conversational message to the journal.
   */
  async recordMessage(agentId: string, message: AgentConversationalMessage): Promise<number> {
    return this.journal.recordMessage(agentId, message);
  }

  /**
   * Forgets a journal entry.
   */
  async forgetEntry(sequence: number): Promise<void> {
    await this.journal.forgetEntry(sequence);
  }

  /**
   * Unforgets a journal entry.
   */
  async unforgetEntry(sequence: number): Promise<void> {
    await this.journal.unforgetEntry(sequence);
  }

  /**
   * Edits a user message.
   */
  async editUserMessage(sequence: number, newContent: string): Promise<void> {
    await this.journal.editUserMessage(sequence, newContent);
  }

  /**
   * Gets the last user message.
   */
  async getLastUserMessage(): Promise<{ sequence: number; content: string } | null> {
    return this.journal.getLastUserMessage();
  }

  /**
   * Gets the last agent turn sequence.
   */
  async getLastAgentTurn(agentId: string): Promise<number | null> {
    return this.journal.getLastAgentTurn(agentId);
  }

  /**
   * Forgets events after a sequence.
   */
  async forgetEventsAfterSequence(sequence: number): Promise<void> {
    await this.journal.forgetEventsAfterSequence(sequence);
  }

  /**
   * Forgets the last turn.
   */
  async forgetLastTurn(): Promise<void> {
    await this.journal.forgetLastTurn();
  }

  /**
   * Gets the journal file path for this thread.
   * For in-memory threads, returns empty string (no file).
   * Used by StateService to read message history.
   */
  public getJournalPath(): string {
    return this.journal.getJournalPath();
  }

  /**
   * Replays journal from a sequence.
   */
  async replayFrom(startSequence: number): Promise<void> {
    const agentFactory = Engine.agentFactory;
    const agents = new Map<string, IAgent>();
    for (const agent of this.agentRegistry.getAll()) {
      agents.set(agent.id, agent);
    }
    await this.journal.replayFrom(startSequence, agentFactory, agents);
  }

  /**
   * Reads all journal entries.
   */
  async readAllEntries(): Promise<InMemoryJournalEntry[]> {
    return this.journal.readAllEntries();
  }

  /**
   * Disposes of the thread resources.
   */
  async dispose(): Promise<void> {
    if (this.isDisposed) return;
    this.isDisposed = true;

    await this.threadManager.dispose();

    const listener = (this as any)._spawnListener;
    if (listener) {
      Engine.eventEmitter.off("agent_spawned", listener);
    }
  }
}
