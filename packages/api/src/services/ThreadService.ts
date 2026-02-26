import { readdir, access, rm } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";
import { Engine, Thread } from "@firmius/core";
import type { AgentWorkType } from "@firmius/shared/types";

const THREADS_BASE_DIR = join(homedir(), ".firmius", "threads");

/**
 * ThreadService - Business logic for thread lifecycle management
 *
 * Provides operations for creating, retrieving, listing, deleting, and
 * managing thread checkpoints with persistent storage.
 */
export class ThreadService {
  private restoringThreads: Map<string, Promise<Thread>> = new Map();

  private async restoreOnce(threadId: string, metadataPath: string): Promise<Thread> {
    const existing = this.restoringThreads.get(threadId);
    if (existing) return existing;

    const promise = Thread.restore(metadataPath).finally(() => {
      this.restoringThreads.delete(threadId);
    });
    this.restoringThreads.set(threadId, promise);
    return promise;
  }
  /**
   * Creates a new thread with the specified configuration.
   *
   * @param config - Thread creation configuration
   * @returns The created Thread instance
   * @throws Error if thread creation fails
   */
   async createThread(config: {
     hostConfig: any;
     rootCwd: string;
     purpose: string;
     objective: string;
     workType?: AgentWorkType;
     generationOptions?: any;
   }): Promise<Thread> {
     try {
       const thread = await Thread.create({
         hostConfig: config.hostConfig,
         rootCwd: config.rootCwd,
         purpose: config.purpose as any,
         objective: config.objective,
         workType: config.workType,
         generationOptions: config.generationOptions,
       });

      return thread;
    } catch (error) {
      console.error("Failed to create thread:", error);
      throw error;
    }
  }

  /**
   * Retrieves a thread by its ID.
   * If not in memory, attempts to restore from disk.
   *
   * @param threadId - The unique identifier of the thread
   * @returns The Thread if found, null otherwise
   */
  async getThread(threadId: string): Promise<Thread | null> {
    const engineThread = Engine.getThread(threadId);
    if (engineThread) {
      return engineThread as unknown as Thread;
    }

    // Try to restore from disk
    try {
      const metadataPath = join(THREADS_BASE_DIR, threadId, "thread-metadata.json");
      const restored = await this.restoreOnce(threadId, metadataPath);
      return restored;
    } catch {
      return null;
    }
  }

  /**
   * Lists all available threads by reading checkpoint files and in-memory threads.
   *
   * @returns Array of all discovered Thread instances
   */
  async listThreads(): Promise<Thread[]> {
    const threads: Thread[] = [];
    const seenThreadIds = new Set<string>();

    // First, get all in-memory threads from Engine
    for (const [threadId, thread] of Engine.threads) {
      threads.push(thread as unknown as Thread);
      seenThreadIds.add(threadId);
    }

    // Then, read checkpoint files for threads not in memory
    try {
      await access(THREADS_BASE_DIR);
    } catch {
      // Directory doesn't exist, return only in-memory threads
      return threads;
    }

    const threadDirs = await readdir(THREADS_BASE_DIR, { withFileTypes: true });

    for (const dir of threadDirs) {
      if (!dir.isDirectory()) continue;

      const threadId = dir.name;
      const metadataPath = join(THREADS_BASE_DIR, threadId, "thread-metadata.json");

      // Skip if we already have this thread in memory
      if (seenThreadIds.has(threadId)) {
        continue;
      }

      try {
        await access(metadataPath);
        const thread = await this.restoreOnce(threadId, metadataPath);
        threads.push(thread);
      } catch (error) {
        // Skip threads with missing or corrupted checkpoints
        console.warn(`Skipping thread ${threadId}: ${error}`);
        continue;
      }
    }

    return threads;
  }

  /**
   * Deletes a thread and all its persisted data.
   *
   * @param threadId - The unique identifier of the thread to delete
   * @returns true if thread was deleted, false if not found
   */
  async deleteThread(threadId: string): Promise<boolean> {
    const thread = Engine.getThread(threadId);

    if (thread) {
      await Engine.removeThread(threadId);
    }

    const threadDir = join(THREADS_BASE_DIR, threadId);

    try {
      await access(threadDir);
    } catch {
      // Thread directory doesn't exist
      return false;
    }

    try {
      await rm(threadDir, { recursive: true, force: true });
      return true;
    } catch (error) {
      console.error(`Failed to delete thread directory ${threadDir}:`, error);
      return false;
    }
  }

  /**
   * Restores a thread from a specific checkpoint file.
   *
   * @param checkpointPath - Path to the checkpoint JSON file
   * @returns The restored Thread instance
   * @throws Error if restoration fails
   */
  async restoreThread(checkpointPath: string): Promise<Thread> {
    try {
      const parts = checkpointPath.split('/');
      const threadIdIndex = parts.indexOf('threads') + 1;
      const threadId = parts[threadIdIndex] as string;
      const thread = await this.restoreOnce(threadId, checkpointPath);
      return thread;
    } catch (error) {
      console.error(`Failed to restore thread from ${checkpointPath}:`, error);
      throw error;
    }
  }

  /**
   * Saves a checkpoint of the thread state to disk.
   *
   * @param thread - The Thread to checkpoint
   * @throws Error if checkpoint fails
   */
  async saveCheckpoint(thread: Thread): Promise<void> {
    try {
      await thread.checkpoint();
    } catch (error) {
      console.error(`Failed to save checkpoint for thread ${thread.id}:`, error);
      throw error;
    }
  }

  /**
   * Creates a new branch for a thread starting from a specific sequence.
   *
   * @param threadId - The ID of the thread to branch
   * @param sequence - The sequence number to branch from
   * @returns The new branch ID
   * @throws Error if thread not found or branching fails
   */
  async branchThread(_threadId: string, _sequence: number): Promise<string> {
    // Branching has been removed in the new per-agent architecture
    throw new Error("Branching is not supported");
  }
}

export default new ThreadService();
