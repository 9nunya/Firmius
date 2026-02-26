import { readFile, mkdir, access, appendFile } from 'node:fs/promises';
import { join } from 'node:path';
import { homedir } from 'node:os';

const CHANGELOG_DIR = join(homedir(), '.firmius', 'changelog');

export interface FileChange {
  type: 'file_edit' | 'file_create' | 'file_delete';
  file: string;
  beforeSnapshotRef: string | null;
  afterSnapshotRef: string | null;
}

export interface ChangeLogEntry {
  sequence: number;
  agentId: string;
  timestamp: number;
  operations: FileChange[];
}

export class ChangeLogService {
  private async ensureDir(dir: string): Promise<void> {
    await mkdir(dir, { recursive: true });
  }

  private getChangelogPath(threadId: string): string {
    return join(CHANGELOG_DIR, `${threadId}.jsonl`);
  }

  async recordChange(threadId: string, entry: ChangeLogEntry): Promise<void> {
    const path = this.getChangelogPath(threadId);
    await this.ensureDir(CHANGELOG_DIR);
    
    const line = JSON.stringify(entry) + '\n';
    await appendFile(path, line, 'utf8');
  }

  async getChangesAfterSequence(
    threadId: string,
    sequence: number
  ): Promise<ChangeLogEntry[]> {
    const path = this.getChangelogPath(threadId);
    const entries: ChangeLogEntry[] = [];

    try {
      await access(path);
      const content = await readFile(path, 'utf8');
      const lines = content.split('\n').filter(line => line.trim());

      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as ChangeLogEntry;
          if (entry.sequence > sequence) {
            entries.push(entry);
          }
        } catch {
          // Skip corrupted lines
        }
      }
    } catch {
      // No changelog for this thread
    }

    return entries.sort((a, b) => a.sequence - b.sequence);
  }

  async getChangesForSequence(
    threadId: string,
    sequence: number
  ): Promise<ChangeLogEntry | null> {
    const path = this.getChangelogPath(threadId);

    try {
      await access(path);
      const content = await readFile(path, 'utf8');
      const lines = content.split('\n').filter(line => line.trim());

      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as ChangeLogEntry;
          if (entry.sequence === sequence) {
            return entry;
          }
        } catch {
          // Skip corrupted lines
        }
      }
    } catch {
      // No changelog for this thread
    }

    return null;
  }

  async getAllChangesForThread(threadId: string): Promise<ChangeLogEntry[]> {
    const path = this.getChangelogPath(threadId);
    const entries: ChangeLogEntry[] = [];

    try {
      await access(path);
      const content = await readFile(path, 'utf8');
      const lines = content.split('\n').filter(line => line.trim());

      for (const line of lines) {
        try {
          const entry = JSON.parse(line) as ChangeLogEntry;
          entries.push(entry);
        } catch {
          // Skip corrupted lines
        }
      }
    } catch {
      // No changelog for this thread
    }

    return entries.sort((a, b) => a.sequence - b.sequence);
  }
}

export const changeLogService = new ChangeLogService();
