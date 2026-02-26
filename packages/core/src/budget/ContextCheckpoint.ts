import * as path from 'node:path';
import * as fs from 'node:fs/promises';
import type { ProviderMessage } from "@firmius/shared/types";
import { type CheckpointData, type BufferEntry } from "./Types";

export class ContextCheckpoint {
  static async create(
    sequence: number,
    threadId: string,
    agentId: string,
    entries: BufferEntry[],
    summary: string,
    preservedFacts: Record<string, string>,
    preservedDecisions: string[]
  ): Promise<CheckpointData> {
    const originalTokens = entries.reduce((sum, e) => sum + e.tokenCount, 0);
    const summaryTokens = Math.ceil(summary.length / 4);

    return {
      sequence,
      threadId,
      agentId,
      timestamp: Date.now(),
      summary,
      preservedFacts,
      preservedDecisions,
      originalTokens,
      summaryTokens,
    };
  }

  static async save(checkpoint: CheckpointData, journalDir: string): Promise<string> {
    await fs.mkdir(journalDir, { recursive: true });
    const filename = `checkpoint-${checkpoint.sequence.toString().padStart(6, '0')}.json`;
    const filepath = path.join(journalDir, filename);
    await Bun.write(filepath, JSON.stringify(checkpoint, null, 2));
    return filepath;
  }

  static async load(filepath: string): Promise<CheckpointData> {
    const content = await Bun.file(filepath).text();
    return JSON.parse(content) as CheckpointData;
  }

  static async loadAll(journalDir: string): Promise<CheckpointData[]> {
    try {
      const files = await fs.readdir(journalDir);
      const checkpointFiles = files
        .filter(f => f.startsWith('checkpoint-') && f.endsWith('.json'))
        .sort();

      const checkpoints: CheckpointData[] = [];
      for (const file of checkpointFiles) {
        const checkpoint = await this.load(path.join(journalDir, file));
        checkpoints.push(checkpoint);
      }

      return checkpoints;
    } catch {
      return [];
    }
  }

  static async delete(checkpoint: CheckpointData, journalDir: string): Promise<void> {
    const filename = `checkpoint-${checkpoint.sequence.toString().padStart(6, '0')}.json`;
    const filepath = path.join(journalDir, filename);
    try {
      await fs.unlink(filepath);
    } catch {
      // Ignore if file doesn't exist
    }
  }

  static toProviderMessage(checkpoints: CheckpointData[]): ProviderMessage[] {
    if (checkpoints.length === 0) return [];

    const summaries = checkpoints.map((cp, i) => {
      let content = `### Checkpoint ${i + 1} (Turn ${cp.sequence})\n`;
      content += cp.summary;

      if (Object.keys(cp.preservedFacts).length > 0) {
        content += '\n\n**Key Facts:**\n';
        for (const [key, value] of Object.entries(cp.preservedFacts)) {
          content += `- ${key}: ${value}\n`;
        }
      }

      if (cp.preservedDecisions.length > 0) {
        content += '\n**Decisions Made:**\n';
        for (const decision of cp.preservedDecisions) {
          content += `- ${decision}\n`;
        }
      }

      return content;
    });

    return [{
      role: 'user',
      content: `## CONTEXT HISTORY (Summarized)\n\nThe following is a summary of previous work that was evicted from context to make room for new operations. Use this to maintain continuity.\n\n${summaries.join('\n\n---\n\n')}`,
    }];
  }

  static getTotalSavedTokens(checkpoints: CheckpointData[]): number {
    return checkpoints.reduce((sum, cp) => sum + (cp.originalTokens - cp.summaryTokens), 0);
  }

  static getLatestCheckpoint(checkpoints: CheckpointData[]): CheckpointData | null {
    if (checkpoints.length === 0) return null;
    return checkpoints[checkpoints.length - 1] ?? null;
  }

  static getCheckpointBySequence(checkpoints: CheckpointData[], sequence: number): CheckpointData | null {
    return checkpoints.find(cp => cp.sequence === sequence) ?? null;
  }
}
