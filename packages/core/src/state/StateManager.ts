import { Database } from "bun:sqlite";
import { mkdir } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";
import type { IHost } from "@firmius/shared";
import { HostType } from "@firmius/shared";

export class StateManager {
  private db: Database;

  private constructor(db: Database, _dbPath: string, _host: IHost, _mountPoint?: string) {
    this.db = db;
  }

  static async create(host: IHost, baseDir: string, threadId: string): Promise<StateManager> {
    const firmiusDir = join(baseDir, ".firmius");
    
    // Ensure .firmius directory exists on the host
    await host.mkdir(firmiusDir);

    if (host.type === HostType.Local) {
      const dbPath = join(firmiusDir, "state.db");
      const db = new Database(dbPath);
      db.run("PRAGMA journal_mode = WAL");
      const sm = new StateManager(db, dbPath, host);
      sm.runMigrations();
      return sm;
    }

    // For Docker/SSH: use a local mount point where the remote .firmius is accessible
    const mountPoint = join(homedir(), ".firmius", "mounts", threadId);
    await mkdir(mountPoint, { recursive: true });

    // Check if already mounted; if not, mount it
    const isMounted = await StateManager.checkMount(mountPoint);
    
    if (!isMounted) {
      if (host.type === HostType.Docker) {
        // Docker: The DockerHost should have mounted container's .firmius to mountPoint
        // We just wait for it to be available
        // The mount is actually done by DockerHost via volume
        // We just need to ensure the directory has content
      } else if (host.type === HostType.RemoteSSH) {
        await StateManager.mountSSHFS(host as any, firmiusDir, mountPoint);
      }
    }

    const dbPath = join(mountPoint, "state.db");
    const db = new Database(dbPath);
    db.run("PRAGMA journal_mode = WAL");
    const sm = new StateManager(db, dbPath, host, mountPoint);
    sm.runMigrations();
    return sm;
  }

  private static async checkMount(mountPoint: string): Promise<boolean> {
    // Check if mount point is actually mounted
    try {
      // On Linux/macOS, check /proc/mounts or use mount command
      const platform = process.platform;
      
      if (platform === 'linux') {
        const result = Bun.spawnSync(['grep', '-q', mountPoint, '/proc/mounts']);
        return result.exitCode === 0;
      } else if (platform === 'darwin') {
        const result = Bun.spawnSync(['mount']);
        if (result.exitCode === 0) {
          return result.stdout.toString().includes(mountPoint);
        }
      }
    } catch {
      // If check fails, assume not mounted
    }
    return false;
  }

  private static mountSSHFS(host: any, remotePath: string, localPath: string): void {
    const sshOptions = host.options || host; // RemoteSSHHost stores config in options property

    // Check sshfs availability
    const whichResult = Bun.spawnSync(['which', 'sshfs']);
    if (whichResult.exitCode !== 0) {
      throw new Error('sshfs is required for SSH hosts. Install: apt install sshfs (Linux) or brew install macfuse (macOS)');
    }

    const hostStr = sshOptions.host;
    const userStr = sshOptions.username ? `${sshOptions.username}@` : '';
    const port = sshOptions.port ? `-p ${sshOptions.port}` : '';
    
    let sshfsCmd: string;
    if (sshOptions.privateKeyPath) {
      sshfsCmd = `sshfs ${userStr}${hostStr}:${remotePath} ${localPath} ${port} -o IdentityFile=${sshOptions.privateKeyPath} -o reconnect -o ServerAliveInterval=15 -o allow_other`;
    } else if (sshOptions.password) {
      // Use sshpass for password auth
      const whichSshpass = Bun.spawnSync(['which', 'sshpass']);
      if (whichSshpass.exitCode !== 0) {
        throw new Error('sshpass is required for password-based SSH. Install: apt install sshpass (Linux) or brew install sshpass (macOS)');
      }
      sshfsCmd = `SSHPASS='${sshOptions.password}' sshpass -e sshfs ${userStr}${hostStr}:${remotePath} ${localPath} ${port} -o password_stdin -o reconnect -o ServerAliveInterval=15 -o allow_other`;
    } else {
      sshfsCmd = `sshfs ${userStr}${hostStr}:${remotePath} ${localPath} ${port} -o reconnect -o ServerAliveInterval=15 -o allow_other`;
    }

    const result = Bun.spawnSync(['sh', '-c', sshfsCmd]);
    if (result.exitCode !== 0) {
      throw new Error(`sshfs mount failed: ${result.stderr}`);
    }
  }

  getDB(): Database {
    return this.db;
  }

  async close(): Promise<void> {
    this.db.close();
    // Note: We do NOT unmount persistent mounts automatically
    // They persist across restarts and need manual cleanup
  }

  private runMigrations(): void {
    // Agents table
    this.db.run(`
      CREATE TABLE IF NOT EXISTS agents (
        id TEXT PRIMARY KEY,
        purpose TEXT NOT NULL,
        parent_id TEXT,
        worktree_path TEXT,
        branch TEXT,
        status TEXT DEFAULT 'initializing',
        spawned_at INTEGER,
        completed_at INTEGER,
        last_heartbeat INTEGER,
        last_progress_update INTEGER,
        last_action_timestamp INTEGER,
        current_task_id TEXT,
        error_message TEXT
      );
    `);

    // Backfill missing columns for older DBs
    const agentColumns = this.db.query(`PRAGMA table_info(agents)`).all() as Array<{ name: string }>;
    const hasLastAction = agentColumns.some((col) => col.name === "last_action_timestamp");
    if (!hasLastAction) {
      this.db.run(`ALTER TABLE agents ADD COLUMN last_action_timestamp INTEGER`);
    }
  }
}
