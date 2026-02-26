import EventEmitter from "node:events";
import type { IAgent } from "@firmius/shared/types";

/**
 * Events emitted by AgentRegistry
 */
export interface AgentRegistryEvents {
  /** Emitted when an agent is added to the registry */
  agent_added: (agent: IAgent) => void;
  /** Emitted when an agent is removed from the registry */
  agent_removed: (agentId: string) => void;
  /** Emitted when the registry is cleared */
  cleared: () => void;
}

/**
 * Event emitter type for AgentRegistry
 */
export type AgentRegistryEventEmitter = EventEmitter & {
  on<U extends keyof AgentRegistryEvents>(event: U, listener: AgentRegistryEvents[U]): AgentRegistryEventEmitter;
  once<U extends keyof AgentRegistryEvents>(event: U, listener: AgentRegistryEvents[U]): AgentRegistryEventEmitter;
  off<U extends keyof AgentRegistryEvents>(event: U, listener: AgentRegistryEvents[U]): AgentRegistryEventEmitter;
  emit<U extends keyof AgentRegistryEvents>(event: U, ...args: Parameters<AgentRegistryEvents[U]>): boolean;
};

/**
 * Thread-scoped registry for managing agents within a thread.
 * 
 * This class provides a thread-local store for agents, allowing retrieval,
 * addition, and removal of agents. It extends Map for standard key-value
 * operations and emits events when agents are added or removed.
 * 
 * @example
 * ```typescript
 * const registry = new AgentRegistry(threadId);
 * registry.set(agent.id, agent);
 * const agents = registry.getAll();
 * const subagents = registry.getByParentId(parentAgentId);
 * ```
 */
export class AgentRegistry extends Map<string, IAgent> {
  private readonly threadId: string;
  private eventEmitter: AgentRegistryEventEmitter;

  /**
   * Creates a new AgentRegistry instance.
   * 
   * @param threadId - The unique identifier of the thread this registry belongs to
   */
  constructor(threadId: string) {
    super();
    this.threadId = threadId;
    this.eventEmitter = new EventEmitter() as AgentRegistryEventEmitter;
  }

  /**
   * Gets the thread ID this registry belongs to.
   */
  public getThreadId(): string {
    return this.threadId;
  }

  /**
   * Retrieves an agent by its ID.
   * 
   * @param id - The unique identifier of the agent
   * @returns The agent if found, undefined otherwise
   */
  public override get(id: string): IAgent | undefined {
    return super.get(id);
  }

  /**
   * Adds or updates an agent in the registry.
   * 
   * @param id - The unique identifier of the agent
   * @param agent - The agent instance to add
   * @returns The registry instance for chaining
   */
  public override set(id: string, agent: IAgent): this {
    const isUpdate = this.has(id);
    super.set(id, agent);
    
    if (!isUpdate) {
      this.eventEmitter.emit("agent_added", agent);
    }
    
    return this;
  }

  /**
   * Removes an agent from the registry.
   * 
   * @param id - The unique identifier of the agent to remove
   * @returns True if the agent was found and removed, false otherwise
   */
  public override delete(id: string): boolean {
    const existed = this.has(id);
    const result = super.delete(id);
    
    if (existed) {
      this.eventEmitter.emit("agent_removed", id);
    }
    
    return result;
  }

  /**
   * Checks if an agent exists in the registry.
   * 
   * @param id - The unique identifier to check
   * @returns True if the agent exists, false otherwise
   */
  public override has(id: string): boolean {
    return super.has(id);
  }

  /**
   * Retrieves all agents in the registry as an array.
   * 
   * @returns Array of all agent instances
   */
  public getAll(): IAgent[] {
    return Array.from(this.values());
  }

  /**
   * Removes all agents from the registry.
   */
  public override clear(): void {
    super.clear();
    this.eventEmitter.emit("cleared");
  }

  /**
   * Retrieves all agents that have the specified parent ID.
   * 
   * @param parentId - The parent agent ID to filter by
   * @returns Array of agents that were spawned by the specified parent
   */
  public getByParentId(parentId: string): IAgent[] {
    return Array.from(this.values()).filter(
      agent => agent.context?.identity.parentId === parentId
    );
  }

  /**
   * Registers an event listener for registry events.
   * 
   * @param event - The event name to listen for
   * @param listener - The callback function to invoke
   */
  public on<U extends keyof AgentRegistryEvents>(
    event: U,
    listener: AgentRegistryEvents[U]
  ): this {
    this.eventEmitter.on(event, listener);
    return this;
  }

  /**
   * Registers a one-time event listener.
   * 
   * @param event - The event name to listen for
   * @param listener - The callback function to invoke
   */
  public once<U extends keyof AgentRegistryEvents>(
    event: U,
    listener: AgentRegistryEvents[U]
  ): this {
    this.eventEmitter.once(event, listener);
    return this;
  }

  /**
   * Removes an event listener.
   * 
   * @param event - The event name
   * @param listener - The callback function to remove
   */
  public off<U extends keyof AgentRegistryEvents>(
    event: U,
    listener: AgentRegistryEvents[U]
  ): this {
    this.eventEmitter.off(event, listener);
    return this;
  }

  /**
   * Gets the number of agents in the registry.
   */
  public override get size(): number {
    return super.size;
  }
}
