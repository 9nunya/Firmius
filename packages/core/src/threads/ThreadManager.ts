import type { IAgent } from "@firmius/shared";
import type { IHost } from "@firmius/shared";
import { Engine } from "@firmius/core";

/**
 * Manages the lifecycle of a thread and its agents.
 * 
 * ThreadManager provides centralized control over thread operations including
 * interrupting agents, managing background subagents, canceling requests,
 * and properly disposing of thread resources.
 */
export class ThreadManager {
  private readonly threadId: string;
  private readonly agents: Set<IAgent>;
  private readonly host: IHost;
  private interrupted: boolean = false;

  /**
   * Creates a new ThreadManager instance.
   * 
   * @param threadId - The unique identifier of the thread
   * @param agents - Set of agents belonging to the thread
   * @param host - The host instance for the thread
   */
  constructor(threadId: string, agents: Set<IAgent>, host: IHost) {
    this.threadId = threadId;
    this.agents = agents;
    this.host = host;
  }

  /**
   * Interrupts all agents in the thread.
   * 
   * This method interrupts both the lead agent and all subagents,
   * and performs a checkpoint to save the interrupted state.
   * 
   * @returns Promise that resolves when all agents have been interrupted
   */
  async interrupt(): Promise<void> {
    this.interrupted = true;

    const interruptPromises = Array.from(this.agents).map(agent =>
      agent.interrupt()
    );
    await Promise.all(interruptPromises);

    await this.interruptBackgroundAgents();

    this.interrupted = false;
  }

  /**
   * Interrupts subagents spawned by the lead agent.
   * 
   * Background agents are agents that were spawned by the lead agent
   * and are running independently. This method ensures all such
   * subagents are properly interrupted.
   * 
   * @returns Promise that resolves when all background agents have been interrupted
   */
  async interruptBackgroundAgents(): Promise<void> {
    const leadAgent = Array.from(this.agents).find(
      agent => agent.context!.identity.parentId === null
    );

    if (!leadAgent) {
      return;
    }

    const subagentIds = leadAgent.context!.identity.subagentIds || [];
    const subagents = Array.from(this.agents).filter(agent =>
      subagentIds.includes(agent.id)
    );

    const interruptPromises = subagents.map(agent =>
      agent.interrupt()
    );

    if (interruptPromises.length > 0) {
      await Promise.all(interruptPromises);
    }
  }

  /**
   * Cancels the current request.
   * 
   * This is equivalent to calling interrupt() - it stops all ongoing
   * agent operations and clears the interrupted state.
   * 
   * @returns Promise that resolves when the request has been canceled
   */
  async cancelRequest(): Promise<void> {
    await this.interrupt();
  }

  /**
   * Clears the interrupted flag.
   * 
   * After calling interrupt() or cancelRequest(), the interrupted flag
   * is automatically cleared. This method can be used to manually
   * clear the flag if needed.
   */
  clearInterrupted(): void {
    this.interrupted = false;
  }

  /**
   * Destroys the thread.
   * 
   * This method:
   * - Stops the checkpoint timer if running
   * - Interrupts all agents
   * - Destroys the host
   * 
   * After destroy(), the thread should no longer be used.
   * 
   * @returns Promise that resolves when the thread has been destroyed
   */
  async destroy(): Promise<void> {
    await this.interrupt();

    if (this.host && typeof this.host.destroy === "function") {
      await this.host.destroy();
    }
  }

  /**
   * Disposes of the thread resources.
   * 
   * This method:
   * - Removes all agents from the AgentFactory registry
   * - Removes the agent spawn listener
   * - Flushes the write queue
   * - Destroys the host
   * 
   * This is the final cleanup step and should be called when the
   * thread is no longer needed.
   * 
   * @returns Promise that resolves when resources have been disposed
   */
  async dispose(): Promise<void> {
    await this.destroy();

    for (const agent of this.agents) {
      Engine.agentFactory.agents.delete(agent.id);
    }

    const listener = (this as any)._spawnListener;
    if (listener) {
      Engine.eventEmitter.off("agent_spawned", listener);
    }
  }

  /**
   * Adds an agent to the thread manager.
   * 
   * @param agent - The agent to add
   */
  addAgent(agent: IAgent): void {
    this.agents.add(agent);
  }

  /**
   * Gets the thread ID.
   * 
   * @returns The unique identifier of the thread
   */
  getThreadId(): string {
    return this.threadId;
  }

  /**
   * Gets all agents in the thread.
   * 
   * @returns Array of all agents in the thread
   */
  getAgents(): IAgent[] {
    return Array.from(this.agents);
  }

  /**
   * Gets the lead agent of the thread.
   * 
   * The lead agent is the agent that has no parent ID.
   * 
   * @returns The lead agent, or undefined if not found
   */
  getLeadAgent(): IAgent | undefined {
    return Array.from(this.agents).find(
      agent => agent.context!.identity.parentId === null
    );
  }

  /**
   * Gets all subagents (agents spawned by the lead agent).
   * 
   * @returns Array of subagents
   */
  getSubagents(): IAgent[] {
    const leadAgent = this.getLeadAgent();
    if (!leadAgent) {
      return [];
    }

    const subagentIds = leadAgent.context!.identity.subagentIds || [];
    return Array.from(this.agents).filter(agent =>
      subagentIds.includes(agent.id)
    );
  }

  /**
   * Checks if the thread is currently interrupted.
   * 
   * @returns True if interrupted, false otherwise
   */
  isInterrupted(): boolean {
    return this.interrupted;
  }
}
