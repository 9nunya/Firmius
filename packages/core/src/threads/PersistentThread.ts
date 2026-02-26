import { readdir, mkdir } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";
import { Engine } from "../index";
import { Coordinator } from "../Coordinator";
import { HostFactory } from "../HostFactory";
import { Agent } from "../Agent";
import { AgentRegistry } from "../registry/AgentRegistry";
import { ThreadManager } from "./ThreadManager";
import { ThreadPersistence, type AgentCheckpoint } from "./ThreadPersistence";
import { AgentJournal } from "./ThreadJournal";
import type { IAgent, AgentTurn, AgentConversationalMessage, AgentContext, HostConfig, IThread } from "@firmius/shared";
import { AgentWorkType, logger } from "@firmius/shared";
import type { AgentSpawnedEvent } from "@firmius/shared/types";

const THREADS_DIR = join(homedir(), ".firmius", "threads");

type AgentStates = Record<string, unknown>;

export class PersistentThread implements IThread {
  public id: string;
  public hostConfig: HostConfig;
  public rootCwd: string;
  public leadAgent: IAgent;
  public coordinator: Coordinator;
  public agentRegistry: AgentRegistry;
  public threadManager: ThreadManager;
  public persistence: ThreadPersistence;
  public threadDir: string;
  public title?: string;
  public interrupted = false;
  public agentStates: AgentStates = {};
  public checkpointedAt: number = Date.now();
  private sequenceCounter: number = 0;
  private checkpointTimer: any;

  private agentJournals: Map<string, AgentJournal> = new Map();

  private constructor(
    id: string,
    hostConfig: HostConfig,
    rootCwd: string,
    leadAgent: IAgent,
    coordinator: Coordinator,
    agentRegistry: AgentRegistry,
    threadManager: ThreadManager,
    persistence: ThreadPersistence,
    threadDir: string,
    title?: string
  ) {
    this.id = id;
    this.hostConfig = hostConfig;
    this.rootCwd = rootCwd;
    this.leadAgent = leadAgent;
    this.coordinator = coordinator;
    this.agentRegistry = agentRegistry;
    this.threadManager = threadManager;
    this.persistence = persistence;
    this.threadDir = threadDir;
    this.title = title;

    this.checkpointTimer = setInterval(() => {
      this.checkpoint().catch(err => console.error("Auto-checkpoint failed:", err));
    }, 60000);
  }

  /**
   * Generates a thread-global monotonically increasing sequence number.
   */
  public nextSequence(): number {
    return this.sequenceCounter++;
  }

  /**
   * Wire up an agent with the thread's persistence callbacks.
   * CRITICAL: Re-hydrates volatile context handlers after restoration.
   */
  private wireAgent(agent: IAgent): void {
    if (!agent.context) return;
    
    agent.context.io.onTurn = async (turn: AgentTurn, agentId: string) => {
      await this.recordTurn(agentId, turn);
    };
    agent.context.io.onCheckpoint = async (agentId: string) => {
      await this.checkpointAgent(agentId);
    };
  }

  public static async create(
    id: string,
    hostConfig: HostConfig,
    rootCwd: string,
    leadAgent: IAgent,
    coordinator: Coordinator,
    agentRegistry: AgentRegistry,
    threadManager: ThreadManager,
    title?: string
  ): Promise<PersistentThread> {
    const threadDir = join(THREADS_DIR, id);
    await mkdir(threadDir, { recursive: true });

    const persistence = ThreadPersistence.forDirectory(threadDir);
    const thread = new PersistentThread(
      id,
      hostConfig,
      rootCwd,
      leadAgent,
      coordinator,
      agentRegistry,
      threadManager,
      persistence,
      threadDir,
      title
    );

    const agentFactory = Engine.agentFactory;

    // Set up lead agent
    thread.wireAgent(leadAgent);
    
    // Set up existing sub-agents if any
    for (const subId of leadAgent.identity.subagentIds) {
      const sub = agentFactory.agents.get(subId) as IAgent | undefined;
      if (sub) thread.wireAgent(sub);
    }

    const onAgentSpawned = (event: AgentSpawnedEvent) => {
      const parentAgent = agentFactory.agents.get(event.parentId!);
      if (event.parentId && thread.agentRegistry.has(parentAgent?.id ?? "")) {
        const newAgent = agentFactory.agents.get(event.agentId) as IAgent | undefined;
        if (newAgent) {
          thread.agentRegistry.set(newAgent.id, newAgent);
          thread.wireAgent(newAgent);
          thread.threadManager.addAgent(newAgent);
        }
      }
    };
    Engine.eventEmitter.on('agent_spawned', onAgentSpawned);
    (thread as any)._spawnListener = onAgentSpawned;

    await thread.checkpoint();
    thread.setupEventListeners();
    return thread;
  }

  public static async restore(metadataPath: string): Promise<PersistentThread> {
    const parts = metadataPath.split('/');
    const threadIdIndex = parts.indexOf('threads') + 1;
    const threadId = parts[threadIdIndex] as string;
    const threadDir = join(THREADS_DIR, threadId);

    const persistence = ThreadPersistence.forDirectory(threadDir);
    const metadata = await persistence.loadThreadMetadata(metadataPath);
    if (!metadata) {
      throw new Error("Thread metadata not found");
    }

    const agentsDir = join(threadDir, "agents");
    await mkdir(agentsDir, { recursive: true });

    const host = await HostFactory.create(metadata.hostConfig as HostConfig);
    await host.init();

    const agentFactory = Engine.agentFactory;
    const agentRegistry = new AgentRegistry(threadId);
    let leadAgent: IAgent | null = null;
    let maxGlobalSequence = 0;

    const entries = await readdir(agentsDir);
    for (const entry of entries) {
      const checkpointPath = join(agentsDir, entry, "checkpoint.json");
      const checkpoint = await persistence.loadAgentCheckpoint(checkpointPath);
      if (!checkpoint) continue;

      const context = checkpoint.context as AgentContext;
      if (!context) continue;

      if (!context.environment.permissions) {
        context.environment.permissions = { scopes: [], allowOutsideCwd: false };
      }
      context.environment.host = host;

      const agent = new Agent(context);
      agentFactory.agents.set(agent.id, agent);
      agentRegistry.set(agent.id, agent);

      if (agent.id === metadata.leadAgentId) {
        leadAgent = agent;
      }

      if (checkpoint.globalSequence !== undefined) {
        maxGlobalSequence = Math.max(maxGlobalSequence, checkpoint.globalSequence);
      }
    }

    if (!leadAgent) {
      throw new Error("Lead agent not found in checkpoints");
    }

    const threadManager = new ThreadManager(threadId, new Set<IAgent>(agentRegistry.getAll()), host);

    const coordinator = await Coordinator.create(host, metadata.rootCwd, threadId);
    const thread = new PersistentThread(
      threadId,
      metadata.hostConfig as HostConfig,
      metadata.rootCwd,
      leadAgent,
      coordinator,
      agentRegistry,
      threadManager,
      persistence,
      threadDir,
      metadata.title
    );

    // RESTORE: Re-wire all agents with persistence callbacks
    for (const agent of agentRegistry.getAll()) {
      thread.wireAgent(agent);
    }

    thread.sequenceCounter = maxGlobalSequence;
    thread.interrupted = false;
    thread.setupEventListeners();
    return thread;
  }

  /**
   * Set up global event listeners for the thread.
   */
  private setupEventListeners(): void {
    const onAgentSpawned = (event: AgentSpawnedEvent) => {
      // Check if this thread owns the parent
      const parentAgent = Engine.agentFactory.agents.get(event.parentId!);
      if (event.parentId && this.agentRegistry.has(parentAgent?.id ?? "")) {
        const newAgent = Engine.agentFactory.agents.get(event.agentId) as IAgent | undefined;
        if (newAgent) {
          this.agentRegistry.set(newAgent.id, newAgent);
          this.wireAgent(newAgent);
          this.threadManager.addAgent(newAgent);
        }
      }
    };
    
    Engine.eventEmitter.on('agent_spawned', onAgentSpawned);
    (this as any)._spawnListener = onAgentSpawned;
  }

  public async checkpoint(): Promise<void> {
    const agents = this.agentRegistry.getAll();
    for (const agent of agents) {
      await this.checkpointAgent(agent.id);
    }

    await this.persistence.saveThreadMetadata({
      version: 1,
      threadId: this.id,
      title: this.title,
      rootCwd: this.rootCwd,
      hostConfig: this.hostConfig,
      leadAgentId: this.leadAgent.id,
      createdAt: Date.now(),
      checkpointedAt: Date.now(),
    }, join(this.threadDir, "thread-metadata.json"));

    this.checkpointedAt = Date.now();
  }

  public async checkpointAgent(agentId: string): Promise<void> {
    const agent = this.agentRegistry.get(agentId);
    if (!agent) return;

    const agentsDir = join(this.threadDir, "agents");
    const agentDir = join(agentsDir, agentId);
    await mkdir(agentDir, { recursive: true });

    const agentJournal = this.agentJournals.get(agentId) ?? await this.getOrCreateAgentJournal(agentId);
    const lastJournalSequence = agentJournal.getSequenceCounter() - 1;

    const agentCtx = agent.context!;
    const checkpoint: AgentCheckpoint = {
      version: 1,
      agentId,
      context: agentCtx,
      historyData: agentCtx.historyData,
      state: agentCtx.state,
      globalSequence: this.sequenceCounter,
      budgetState: {
        checkpoints: (agentCtx.state as any).checkpoints || [],
        protectedContext: (agentCtx.state as any).protectedContext || {},
      } as any,
      checkpointedAt: Date.now(),
      lastJournalSequence,
    };

    await this.persistence.saveAgentCheckpoint(checkpoint, join(agentDir, "checkpoint.json"));
  }

  public async recordTurn(agentId: string, turn: AgentTurn): Promise<number> {
    const sequence = this.nextSequence();
    const agentJournal = await this.getOrCreateAgentJournal(agentId);
    await agentJournal.recordTurn(agentId, turn, sequence);

    const targetAgent = this.agentRegistry.get(agentId);
    if (targetAgent && targetAgent.context) {
      targetAgent.context.state.metrics.lastTurnTokens = turn.tokens;
      targetAgent.context.state.metrics.totalTokens += turn.tokens;

      // CRITICAL: Update in-memory history to prevent amnesia
      const history = targetAgent.context.historyData.history;
      if (history.type === AgentWorkType.Goal) {
        if (!history.workflow) {
          history.workflow = { turns: [], timestamp: Date.now(), completed: false, finalMessage: null };
        }
        history.workflow.turns.push(turn);
      } else if (history.type === AgentWorkType.Conversational) {
        if (!history.conversation) {
          history.conversation = { history: [] };
        }
        // Conversational turns with tool calls are treated as internal workflow steps
        history.conversation.history.push({
          turns: [turn],
          timestamp: turn.timestamp,
          completed: true,
          finalMessage: typeof turn.content === 'string' ? turn.content : (Array.isArray(turn.content) ? turn.content.map((p: any) => p.text || '').join('') : null)
        });
      }
    } else {
      logger.warn(`[PersistentThread] Could not find agent ${agentId} in registry to update memory. Memory desync may occur.`);
    }

    return sequence;
  }

  public async recordMessage(agentId: string, message: AgentConversationalMessage): Promise<number> {
    const sequence = this.nextSequence();
    const agentJournal = await this.getOrCreateAgentJournal(agentId);
    await agentJournal.recordMessage(agentId, message, sequence);

    const targetAgent = this.agentRegistry.get(agentId);
    if (targetAgent && targetAgent.context) {
      targetAgent.context.state.metrics.totalTokens += (message.tokens || 0);

      // CRITICAL: Update in-memory history to prevent amnesia
      const history = targetAgent.context.historyData.history;
      if (history.type === AgentWorkType.Conversational) {
        if (!history.conversation) {
          history.conversation = { history: [] };
        }
        history.conversation.history.push(message);
      } else if (history.type === AgentWorkType.Goal) {
        if (!history.conversation) {
          history.conversation = { history: [] };
        }
        history.conversation.history.push(message);
      }
    }

    return sequence;
  }

  public async getAgentJournalEntries(agentId: string): Promise<any[]> {
    const journal = await this.getOrCreateAgentJournal(agentId);
    return await journal.readAllEntries();
  }

  public getAllAgentIds(): string[] {
    return this.agentRegistry.getAll().map((a: any) => a.id);
  }

  private async getOrCreateAgentJournal(agentId: string): Promise<AgentJournal> {
    let journal = this.agentJournals.get(agentId);
    if (!journal) {
      const journalPath = join(this.threadDir, "agents", agentId, "journal.jsonl");
      await mkdir(join(this.threadDir, "agents", agentId), { recursive: true });
      journal = new AgentJournal(journalPath, 0, agentId);
      await journal.ensureInitialized();
      this.agentJournals.set(agentId, journal);
    }
    return journal;
  }

  public async clearInterrupted(): Promise<void> {
    this.interrupted = false;
    await this.threadManager.clearInterrupted();
  }

  public async forgetEntry(sequence: number): Promise<void> {
    for (const journal of this.agentJournals.values()) {
      await journal.forgetEntry(sequence).catch(() => {});
    }
  }

  public async unforgetEntry(sequence: number): Promise<void> {
    for (const journal of this.agentJournals.values()) {
      await journal.unforgetEntry(sequence);
    }
  }

  public async editUserMessage(sequence: number, newContent: string): Promise<void> {
    for (const journal of this.agentJournals.values()) {
      await journal.editUserMessage(sequence, newContent).catch(() => {});
    }
  }

  public async forgetEventsAfterSequence(sequence: number): Promise<void> {
    for (const journal of this.agentJournals.values()) {
      await journal.forgetEventsAfterSequence(sequence);
    }
  }

  public getSubagents(): IAgent[] {
    return this.threadManager.getSubagents();
  }

  public async interrupt(): Promise<void> { await this.threadManager.interrupt(); }
  public async cancelRequest(): Promise<void> { await this.threadManager.cancelRequest(); }
  public async dispose(): Promise<void> { 
    if (this.checkpointTimer) clearInterval(this.checkpointTimer);
    if ((this as any)._spawnListener) {
      Engine.eventEmitter.off('agent_spawned', (this as any)._spawnListener);
    }
    await this.threadManager.dispose(); 
  }
  public async getLastUserMessage(): Promise<any> {
    for (const journal of this.agentJournals.values()) {
      const msg = await journal.getLastUserMessage();
      if (msg) return msg;
    }
    return null;
  }

  public async getLastAgentTurn(agentId: string): Promise<number | null> {
    const journal = await this.getOrCreateAgentJournal(agentId);
    return await journal.getLastAgentTurn(agentId);
  }

  public async forgetLastTurn(): Promise<void> {
    for (const journal of this.agentJournals.values()) {
      await journal.forgetLastTurn().catch(() => {});
    }
  }

  public getJournalPath(): string {
    return join(this.threadDir, "thread-journal.jsonl");
  }
}
