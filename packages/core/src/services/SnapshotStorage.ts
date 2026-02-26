import { writeFile, readFile, mkdir, access, readdir } from 'node:fs/promises';
import { join } from 'node:path';
import { homedir } from 'node:os';
import type { LSPDiagnostic } from '@firmius/shared';

const SNAPSHOTS_DIR = join(homedir(), '.firmius', 'snapshots');

export interface FileEditSnapshot {
  threadId: string;
  agentId: string;
  turnIndex: number;
  toolCallId: string;
  file: string;
  operation: string;
  beforeContent: string | null;
  afterContent: string;
  timestamp: number;
  hunks?: Array<{ startLine: number; endLine: number; action: string }>;
  diagnostics?: LSPDiagnostic[];
}

export class SnapshotStorage {
  private async ensureDir(dir: string): Promise<void> {
    await mkdir(dir, { recursive: true });
  }

  private getSnapshotPath(
    threadId: string,
    agentId: string,
    turnIndex: number,
    toolCallId: string
  ): string {
    return join(
      SNAPSHOTS_DIR,
      threadId,
      agentId,
      `${turnIndex}-${toolCallId}.json`
    );
  }

  async storeSnapshot(snapshot: FileEditSnapshot): Promise<void> {
    const path = this.getSnapshotPath(
      snapshot.threadId,
      snapshot.agentId,
      snapshot.turnIndex,
      snapshot.toolCallId
    );

    await this.ensureDir(join(SNAPSHOTS_DIR, snapshot.threadId, snapshot.agentId));
    await writeFile(path, JSON.stringify(snapshot, null, 2), 'utf8');
  }

  async getSnapshot(
    threadId: string,
    agentId: string,
    turnIndex: number,
    toolCallId: string
  ): Promise<FileEditSnapshot | null> {
    const path = this.getSnapshotPath(threadId, agentId, turnIndex, toolCallId);

    try {
      await access(path);
      const content = await readFile(path, 'utf8');
      return JSON.parse(content) as FileEditSnapshot;
    } catch {
      return null;
    }
  }

  async getAllSnapshotsForThread(threadId: string): Promise<FileEditSnapshot[]> {
    const threadDir = join(SNAPSHOTS_DIR, threadId);
    const snapshots: FileEditSnapshot[] = [];

    try {
      await access(threadDir);
      
      // Read all agent directories
      const agentDirs = await readdir(threadDir, { withFileTypes: true });
      
      for (const agentDir of agentDirs) {
        if (!agentDir.isDirectory()) continue;
        
        const agentPath = join(threadDir, agentDir.name);
        const files = await readdir(agentPath, { withFileTypes: true });
        
        // Read all snapshot files in this agent directory
        for (const file of files) {
          if (!file.isFile() || !file.name.endsWith('.json')) continue;
          
          try {
            const content = await readFile(join(agentPath, file.name), 'utf8');
            const snapshot = JSON.parse(content) as FileEditSnapshot;
            snapshots.push(snapshot);
          } catch {
            // Skip corrupted snapshot files
          }
        }
      }
      
      // Sort by timestamp
      snapshots.sort((a, b) => a.timestamp - b.timestamp);
    } catch {
      // No snapshots for this thread
    }

    return snapshots;
  }
}

export const snapshotStorage = new SnapshotStorage();
