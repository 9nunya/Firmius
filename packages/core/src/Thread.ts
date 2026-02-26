import { randomUUID } from "node:crypto";
import type { IThread } from "@firmius/shared";
import {
  type IAgent,
  type AgentTurn,
  type AgentConversationalMessage,
  type AgentPurpose,
  AgentWorkType,
  type HostConfig,
} from "@firmius/shared";
import { PersistentThread } from "./threads/PersistentThread";
import { InMemoryThread } from "./threads/InMemoryThread";
import { AgentRegistry } from "./registry/AgentRegistry";
import { ThreadManager } from "./threads/ThreadManager";
import { HostFactory } from "./HostFactory";
import { Coordinator } from "./Coordinator";
import { Engine } from "./Engine";

export interface ThreadCreateOptions {
  hostConfig: HostConfig;
  rootCwd: string;
  purpose: AgentPurpose;
  objective: string;
  workType?: AgentWorkType;
  generationOptions?: {
    providerId: string;
    modelId: string;
    reasoningEffort?: string;
    maxTokens?: number;
  };
  parentId?: string;
  /** If true, creates an in-memory thread without persistence */
  inMemory?: boolean;
}

/**
 * Thread - A facade class that delegates all operations to the underlying thread instance.
 */
export class Thread {
  /** The underlying thread instance (either PersistentThread or InMemoryThread) */
  private readonly thread: IThread;

  /**
   * Private constructor - use static create() or restore() methods instead.
   *
   * @param thread - The underlying thread instance to wrap
   */
  private constructor(thread: IThread) {
    this.thread = thread;
  }

  /**
   * Creates a new Thread instance.
   *
   * @param options - Options for creating the thread
   * @returns A new Thread instance wrapping either PersistentThread or InMemoryThread
   */
  static async create(options: ThreadCreateOptions): Promise<Thread> {
    let thread: IThread;
    if (options.inMemory) {
      thread = await InMemoryThread.create(options as any);
    } else {
      // Create lead agent via Engine's agent factory
      const host = await HostFactory.create(options.hostConfig);
      await host.init();
      
      const threadId = randomUUID();
      const leadAgent = await Engine.agentFactory.summon({
        purpose: options.purpose,
        objective: options.objective,
        cwd: options.rootCwd,
        host,
        workType: options.workType || AgentWorkType.Conversational,
        generationOptions: options.generationOptions as any,
        parentId: options.parentId,
        threadId,
      });

      const agentRegistry = new AgentRegistry(threadId);
      agentRegistry.set(leadAgent.id, leadAgent);

      const threadManager = new ThreadManager(threadId, new Set<IAgent>([leadAgent]), host);
      const coordinator = await Coordinator.create(host, options.rootCwd, threadId);

      thread = await PersistentThread.create(
        threadId,
        options.hostConfig,
        options.rootCwd,
        leadAgent,
        coordinator,
        agentRegistry,
        threadManager,
        options.objective.split('\n')[0]
      );
    }
    
    const existing = Engine.getThread(thread.id);
    if (existing) {
      return new Thread(existing);
    }
    
    Engine.addThread(thread);
    return new Thread(thread);
  }

  /**
   * Restores a Thread from a checkpoint file.
   *
   * @param checkpointPath - Path to the checkpoint JSON file
   * @returns A restored Thread instance
   */
  static async restore(checkpointPath: string): Promise<Thread> {
    const parts = checkpointPath.split('/');
    const threadIdIndex = parts.indexOf('threads') + 1;
    const threadId = parts[threadIdIndex] as string;
    
    const existing = Engine.getThread(threadId);
    if (existing) {
      return new Thread(existing);
    }
    
    const thread = await PersistentThread.restore(checkpointPath);
    Engine.addThread(thread);
    return new Thread(thread);
  }

  // ==================== Getters that delegate to underlying thread ====================

  /**
   * Gets the unique identifier of the thread.
   */
  get id(): string {
    return this.thread.id;
  }

  /**
   * Gets the optional title of the thread.
   */
  get title(): string | undefined {
    return (this.thread as any).title;
  }

  /**
   * Sets the title of the thread.
   */
  set title(value: string | undefined) {
    (this.thread as any).title = value;
  }

  /**
   * Gets the root working directory of the thread.
   */
  get rootCwd(): string {
    return this.thread.rootCwd;
  }

  /**
   * Gets the host configuration for the thread.
   */
  get hostConfig(): HostConfig {
    return this.thread.hostConfig;
  }

  /**
   * Gets the lead agent of the thread.
   */
  get leadAgent(): IAgent {
    return this.thread.leadAgent;
  }

  /**
   * Gets the agent registry for the thread.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  get agents(): AgentRegistry {
    return (this.thread as any).agentRegistry;
  }

  /**
   * Gets the total tokens used by the lead agent.
   */
  get tokensUsed(): number {
    return (this.thread as any).tokensUsed ?? 0;
  }

  /**
   * Gets the timestamp of the last checkpoint.
   */
  get checkpointedAt(): Date {
    const ts = (this.thread as any).checkpointedAt;
    return ts ? new Date(ts) : new Date();
  }

  /**
   * Gets whether the thread is in an interrupted state.
   */
  get interrupted(): boolean {
    return (this.thread as any).interrupted;
  }

  /**
   * Sets the interrupted state of the thread.
   */
  set interrupted(value: boolean) {
    (this.thread as any).interrupted = value;
  }

  // ==================== Methods that delegate to underlying thread ====================

  /**
   * Gets all subagents of the lead agent.
   */
  getSubagents(): IAgent[] {
    return this.thread.getSubagents();
  }

  /**
   * Gets all agent IDs in the thread.
   */
  getAllAgentIds(): string[] {
    return this.thread.getAllAgentIds();
  }

  /**
   * Performs a checkpoint of the thread state.
   */
  async checkpoint(): Promise<void> {
    await this.thread.checkpoint();
  }

  async dispose(): Promise<void> {
    Engine.removeThread(this.thread.id);
    await this.thread.dispose();
  }

  /**
   * Destroys the thread and releases all resources.
   * Alias for dispose() for backward compatibility.
   */
  async destroy(): Promise<void> {
    await this.dispose();
  }

  /**
   * Interrupts all agents in the thread.
   */
  async interrupt(): Promise<void> {
    await this.thread.interrupt();
  }

  /**
   * Cancels the current request.
   */
  async cancelRequest(): Promise<void> {
    await this.thread.cancelRequest();
  }

  /**
   * Clears the interrupted state.
   * Note: This is only available on PersistentThread.
   */
  clearInterrupted(): void {
    const thread = this.thread as any;
    if (typeof thread.clearInterrupted === "function") {
      thread.clearInterrupted();
    } else {
      thread.interrupted = false;
    }
  }

  /**
   * Records an agent turn to the journal.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async recordTurn(agentId: string, turn: AgentTurn): Promise<number> {
    return (this.thread as any).recordTurn(agentId, turn);
  }

  /**
   * Records a conversational message to the journal.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async recordMessage(
    agentId: string,
    message: AgentConversationalMessage,
  ): Promise<number> {
    return (this.thread as any).recordMessage(agentId, message);
  }

  /**
   * Marks an entry as forgotten.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async forgetEntry(sequence: number): Promise<void> {
    await (this.thread as any).forgetEntry(sequence);
  }

  /**
   * Removes the forgotten marker from an entry.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async unforgetEntry(sequence: number): Promise<void> {
    await (this.thread as any).unforgetEntry(sequence);
  }

  /**
   * Edits a user message.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async editUserMessage(sequence: number, newContent: string): Promise<void> {
    await (this.thread as any).editUserMessage(sequence, newContent);
  }

  /**
   * Gets the last user message from the journal.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async getLastUserMessage(): Promise<{
    sequence: number;
    content: string;
  } | null> {
    return (this.thread as any).getLastUserMessage();
  }

  /**
   * Gets the last agent turn for a specific agent.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async getLastAgentTurn(agentId: string): Promise<number | null> {
    return (this.thread as any).getLastAgentTurn(agentId);
  }

  /**
   * Forgets all events after a specific sequence number.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async forgetEventsAfterSequence(targetSequence: number): Promise<void> {
    await (this.thread as any).forgetEventsAfterSequence(targetSequence);
  }

  /**
   * Forgets last agent turn.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  async forgetLastTurn(): Promise<void> {
    await (this.thread as any).forgetLastTurn();
  }

  /**
   * Gets the journal file path for this thread.
   * Note: This is only available on PersistentThread and InMemoryThread.
   */
  getJournalPath(): string {
    return (this.thread as any).getJournalPath();
  }

  /**
   * Gets all journal entries for a specific agent.
   * Note: This is only available on PersistentThread.
   */
  async getAgentJournalEntries(agentId: string): Promise<any[]> {
    if (typeof (this.thread as any).getAgentJournalEntries === "function") {
      return (this.thread as any).getAgentJournalEntries(agentId);
    }
    return [];
  }
}
