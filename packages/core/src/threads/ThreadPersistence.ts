import { writeFile, readFile, mkdir, unlink } from "node:fs/promises";
import { rename } from "node:fs/promises";
import { existsSync } from "node:fs";
import { dirname } from "node:path";
import { WriteQueue } from "@firmius/shared";
import type { BudgetState, CheckpointData } from "@firmius/core/budget";

export interface ThreadCheckpoint {
  version: number;
  threadId: string;
  hostConfig: unknown;
  rootCwd: string;
  leadAgentId: string;
  agents: Array<{
    context: unknown;
    subagentIds: string[];
  }>;
  lastJournalSequence: number;
  checkpointedAt: number;
  wasInterrupted: boolean;
  agentStates: Record<string, unknown>;
  budgetState?: {
    budget: BudgetState;
    checkpoints: CheckpointData[];
    protectedContext: {
      objective: string;
      anchors: string[];
      recentUserMessages: Array<{ type: string; content: string; timestamp: number }>;
      keyFacts: Record<string, string>;
    };
  };
}

export interface AgentCheckpoint {
  version: number;
  agentId: string;
  context: unknown;
  historyData: unknown;
  state: unknown;
  budgetState: {
    budget?: BudgetState;
    checkpoints: CheckpointData[];
    protectedContext?: {
      objective: string;
      anchors: string[];
      recentUserMessages: Array<{ type: string; content: string; timestamp: number }>;
      keyFacts: Record<string, string>;
    };
  };
  checkpointedAt: number;
  lastJournalSequence: number;
  globalSequence?: number;
}

export interface ThreadMetadata {
  version: number;
  threadId: string;
  title?: string;
  rootCwd: string;
  hostConfig: unknown;
  leadAgentId: string;
  createdAt: number;
  checkpointedAt: number;
}

/**
 * Interface representing a journal entry in the thread journal.
 */
interface JournalEntry {
  sequence: number;
  agentId: string;
  timestamp: number;
  type: "turn" | "message" | "forgotten" | "message_edited";
  payload: unknown;
}

/**
 * ThreadPersistence - Handles persisting thread state to disk with atomic writes
 * and cleanup of old forgotten journal entries.
 * 
 * Uses WriteQueue to serialize write operations and prevent race conditions.
 * 
 * This class is a singleton per thread directory to ensure all concurrent
 * operations on the same thread use the same WriteQueue instance.
 */
export class ThreadPersistence {
  private writeQueue: WriteQueue;
  private static instances: Map<string, ThreadPersistence> = new Map();

  private constructor(_threadDir: string) {
    this.writeQueue = new WriteQueue();
  }

  static forDirectory(threadDir: string): ThreadPersistence {
    let instance = ThreadPersistence.instances.get(threadDir);
    if (!instance) {
      instance = new ThreadPersistence(threadDir);
      ThreadPersistence.instances.set(threadDir, instance);
    }
    return instance;
  }

  static removeDirectory(threadDir: string): void {
    ThreadPersistence.instances.delete(threadDir);
  }

  /**
   * Saves a checkpoint to disk using atomic write (write to temp file then rename).
   * Uses WriteQueue to serialize checkpoint writes.
   * 
   * @param checkpoint - The checkpoint data to save
   * @param path - Optional path to save the checkpoint. If not provided, uses default location
   */
  async saveCheckpoint(checkpoint: ThreadCheckpoint, path?: string): Promise<void> {
    const checkpointPath = path;
    if (!checkpointPath) {
      throw new Error("Checkpoint path is required");
    }

    await this.writeQueue.run(async () => {
      const dir = dirname(checkpointPath);
      if (!existsSync(dir)) {
        await mkdir(dir, { recursive: true });
      }

      const content = JSON.stringify(checkpoint, null, 2);
      await this.atomicWrite(checkpointPath, content);
    });
  }

  /**
   * Loads a checkpoint from disk.
   * 
   * @param path - The path to the checkpoint file
   * @returns The loaded checkpoint data
   */
  async loadCheckpoint(path: string): Promise<ThreadCheckpoint> {
    const content = await readFile(path, "utf8");
    return JSON.parse(content) as ThreadCheckpoint;
  }

  /**
   * Writes content to a file atomically by writing to a temporary file
   * then renaming it to the target path.
   * 
   * @param path - The target file path
   * @param content - The content to write
   */
  async atomicWrite(path: string, content: string): Promise<void> {
    const tmpPath = path + ".tmp";
    const dir = dirname(path);

    await mkdir(dir, { recursive: true });
    await writeFile(tmpPath, content, "utf8");

    try {
      await rename(tmpPath, path);
    } catch (err: any) {
      if (err.code === "ENOENT") {
        await mkdir(dir, { recursive: true });
        try {
          await rename(tmpPath, path);
          return;
        } catch (retryErr: any) {
          await unlink(tmpPath).catch(() => {});
          throw new Error(`Atomic write failed after retry: ${path} (${retryErr.code})`);
        }
      }
      await unlink(tmpPath).catch(() => {});
      throw err;
    }
  }

  /**
   * Purges old forgotten entries from a journal file.
   * 
   * Entries that have been marked as "forgotten" are kept for a period of time
   * (TTL) before being permanently removed. This method removes forgotten entries
   * older than the specified TTL and also removes the entries they reference.
   * 
   * @param journalPath - Path to the journal file
   * @param ttlMs - Time-to-live in milliseconds for forgotten entries
   */
  async purgeOldForgottenEntries(journalPath: string, ttlMs: number): Promise<void> {
    const now = Date.now();

    try {
      const content = await readFile(journalPath, "utf8");
      const lines = content.split("\n").filter(line => line.trim());
      const toDelete = new Set<number>();

      // First pass: find forgotten markers older than TTL
      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as JournalEntry;
          if (entry.type === "forgotten") {
            const forgottenAt = (entry.payload as { forgottenAt: number }).forgottenAt;
            const age = now - forgottenAt;
            if (age > ttlMs) {
              const targetSequence = (entry.payload as { targetSequence: number }).targetSequence;
              toDelete.add(targetSequence);
            }
          }
        } catch {
          // Ignore parse errors
        }
      }

      if (toDelete.size === 0) {
        return;
      }

      // Second pass: filter out old forgotten entries and their targets
      const filtered = lines.filter(line => {
        try {
          const entry = JSON.parse(line) as JournalEntry;

          // Remove the forgotten marker itself if its target is being deleted
          if (entry.type === "forgotten" && toDelete.has((entry.payload as { targetSequence: number }).targetSequence)) {
            return false;
          }

          // Remove the entry that was forgotten
          if (toDelete.has(entry.sequence)) {
            return false;
          }

          return true;
        } catch {
          return true;
        }
      });

      await this.writeQueue.run(async () => {
        await writeFile(journalPath, filtered.join("\n") + "\n", "utf8");
      });
    } catch {
      // Journal doesn't exist yet, nothing to purge
    }
  }

  // ==================== Agent Checkpoint Methods ====================

  /**
   * Saves an agent checkpoint to disk.
   *
   * @param checkpoint - The agent checkpoint data to save
   * @param path - Path to save the checkpoint file
   */
  async saveAgentCheckpoint(checkpoint: AgentCheckpoint, path: string): Promise<void> {
    await this.writeQueue.run(async () => {
      const dir = dirname(path);
      if (!existsSync(dir)) {
        await mkdir(dir, { recursive: true });
      }
      const content = JSON.stringify(checkpoint, null, 2);
      await this.atomicWrite(path, content);
    });
  }

  /**
   * Loads an agent checkpoint from disk.
   *
   * @param path - Path to the agent checkpoint file
   * @returns The loaded checkpoint data, or null if not found
   */
  async loadAgentCheckpoint(path: string): Promise<AgentCheckpoint | null> {
    try {
      const content = await readFile(path, "utf8");
      return JSON.parse(content) as AgentCheckpoint;
    } catch {
      return null;
    }
  }

  // ==================== Thread Metadata Methods ====================

  /**
   * Saves thread metadata to disk.
   *
   * @param metadata - The thread metadata to save
   * @param path - Path to save the metadata file (typically thread-metadata.json)
   */
  async saveThreadMetadata(metadata: ThreadMetadata, path: string): Promise<void> {
    await this.writeQueue.run(async () => {
      const dir = dirname(path);
      if (!existsSync(dir)) {
        await mkdir(dir, { recursive: true });
      }
      const content = JSON.stringify(metadata, null, 2);
      await this.atomicWrite(path, content);
    });
  }

  /**
   * Loads thread metadata from disk.
   *
   * @param path - Path to the thread metadata file
   * @returns The loaded metadata, or null if not found
   */
  async loadThreadMetadata(path: string): Promise<ThreadMetadata | null> {
    try {
      const content = await readFile(path, "utf8");
      return JSON.parse(content) as ThreadMetadata;
    } catch {
      return null;
    }
  }
}
