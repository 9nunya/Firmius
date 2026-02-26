import { logger } from "@firmius/shared/utils";

import { join } from 'node:path';
import * as fs from 'node:fs';
import {
  HostProcessFinishReason,
  HostType,
  type DockerHostOptions,
  type HostExecOptions,
  type HostFileEntry,
  type HostListDirOptions,
  type HostProcessHandle,
  type HostProcessResult,
  type IHost
} from '@firmius/shared/types';


export class DockerHost implements IHost {
  type = HostType.Docker;
  defaultCwd = '/work';
  private image: string;
  private containerName: string;
  private volumes: Record<string, string>;
  private env: Record<string, string>;
  private bootstrapScript?: string;
  private bootstrapScriptPath?: string;
  private repo?: string;
  private containerRunning = false;

  constructor(options: DockerHostOptions) {
    this.image = options.image;
    this.containerName = options.containerName;
    this.volumes = options.volumes || {};
    this.env = options.env || {};
    this.bootstrapScript = options.bootstrapScript;
    this.bootstrapScriptPath = options.bootstrapScriptPath;
    this.repo = options.repo;
  }

  async init(): Promise<void> {
    if (this.containerRunning) return;

    // Check if container already exists
    const checkPs = await this.execLocal(`docker ps -a --filter "name=^/${this.containerName}$" --format "{{.Names}}"`);
    if (checkPs.stdout.trim() === this.containerName) {
      logger.info(`[DockerHost] Container ${this.containerName} exists. Checking status...`);
      const checkRunning = await this.execLocal(`docker ps --filter "name=^/${this.containerName}$" --format "{{.Names}}"`);
      if (checkRunning.stdout.trim() !== this.containerName) {
        logger.info(`[DockerHost] Container ${this.containerName} is stopped. Starting...`);
        await this.execLocal(`docker start ${this.containerName}`);
      }
    } else {
      // Create and start
      logger.info(`[DockerHost] Creating new container: ${this.containerName}`);
      const volumeArgs = Object.entries(this.volumes)
        .map(([host, guest]) => `-v "${host}:${guest}"`)
        .join(' ');
      const envArgs = Object.entries(this.env)
        .map(([key, val]) => `-e "${key}=${val}"`)
        .join(' ');

      await this.execLocal(
        `docker run -d --name ${this.containerName} ${volumeArgs} ${envArgs} ${this.image} tail -f /dev/null`
      );
    }

    this.containerRunning = true;

    // Run bootstrap
    if (this.bootstrapScript) {
      await this.exec(this.bootstrapScript, { cwd: '/' });
    }
    if (this.bootstrapScriptPath) {
      await this.runBootstrapScript(this.bootstrapScriptPath);
    }

    // Clone repo if provided
    if (this.repo) {
      try {
        // Ensure git is available; attempt to clone into current working directory
        const cloneCmd = `git clone ${this.repo} .`;
        const res = await this.exec(cloneCmd, { cwd: this.defaultCwd });
        if (res.exitCode !== 0) {
          logger.warn(`[DockerHost] git clone failed: ${res.stderr}`);
        } else {
          logger.info(`[DockerHost] Cloned repo ${this.repo} into ${this.defaultCwd}`);
        }
      } catch (e: any) {
        logger.warn(`[DockerHost] git clone error: ${e.message}`);
      }
    }
  }

  async destroy(): Promise<void> {
    if (!this.containerRunning) return;
    await this.execLocal(`docker rm -f ${this.containerName}`);
    this.containerRunning = false;
  }

  async exec(command: string, options?: HostExecOptions): Promise<HostProcessResult> {
    if (!this.containerRunning) await this.init();

    const startTime = Date.now();
    const args = ['docker', ...this.buildDockerArgs(command, {
      cwd: options?.cwd || this.defaultCwd,
      ...options,
      pty: false
    })];

    const proc = Bun.spawn(args, {
      env: { ...process.env, ...options?.env },
      timeout: options?.timeout,
      stdout: 'pipe',
      stderr: 'pipe'
    });

    const [exitCode, stdoutData, stderrData] = await Promise.all([
      proc.exited,
      (async () => {
        try {
          const stdout = proc.stdout as unknown as ReadableStream;
          const reader = stdout?.getReader();
          if (!reader) return '';
          const decoder = new TextDecoder();
          let result = '';
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            result += decoder.decode(value);
          }
          return result;
        } catch (e) {
          logger.error("[DockerHost] Error reading stdout: " + e);
          return '';
        }
      })(),
      (async () => {
        try {
          const stderr = proc.stderr as unknown as ReadableStream;
          const reader = stderr?.getReader();
          if (!reader) return '';
          const decoder = new TextDecoder();
          let result = '';
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            result += decoder.decode(value);
          }
          return result;
        } catch (e) {
          logger.error("[DockerHost] Error reading stderr: " + e);
          return '';
        }
      })()
    ]);

    const stdout = stdoutData;
    const stderr = stderrData;
    const elapsed = Date.now() - startTime;

    let finishReason = HostProcessFinishReason.Natural;
    if (exitCode === null) {
      finishReason = (options?.timeout && elapsed >= options.timeout)
        ? HostProcessFinishReason.Timeout
        : HostProcessFinishReason.Killed;
    }

    return { exitCode: exitCode ?? -1, stdout, stderr, elapsed, finishReason };
  }

  async spawn(command: string, options?: HostExecOptions): Promise<HostProcessHandle> {
    if (!this.containerRunning) await this.init();

    const startTime = Date.now();
    const listeners = new Set<(data: string, source: 'stdout' | 'stderr') => void>();

    const state = {
      stdout: '',
      stderr: '',
      completed: false,
      exitCode: null as number | null
    };

    const usePty = options?.pty !== false;
    const spawnOptions: any = {
      env: { ...process.env, ...options?.env },
      timeout: options?.timeout,
    };

    if (usePty) {
      spawnOptions.terminal = {
        cols: 80,
        rows: 24,
        data(_terminal: any, data: Uint8Array) {
          const chunk = new TextDecoder().decode(data);
          state.stdout += chunk;
          for (const listener of listeners) {
            listener(chunk, 'stdout');
          }
        },
      };
    } else {
      spawnOptions.stdout = 'pipe';
      spawnOptions.stderr = 'pipe';
      spawnOptions.stdin = 'pipe';
    }

    const proc = Bun.spawn(['docker', ...this.buildDockerArgs(command, options)], spawnOptions);

    if (!usePty) {
      const readStdout = async () => {
        const stdout = proc.stdout as unknown as ReadableStream;
        const reader = stdout?.getReader();
        if (!reader) return;
        const decoder = new TextDecoder();
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          const chunk = decoder.decode(value);
          state.stdout += chunk;
          for (const listener of listeners) {
            listener(chunk, 'stdout');
          }
        }
      };

      const readStderr = async () => {
        const stderr = proc.stderr as unknown as ReadableStream;
        const reader = stderr?.getReader();
        if (!reader) return;
        const decoder = new TextDecoder();
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          const chunk = decoder.decode(value);
          state.stderr += chunk;
          for (const listener of listeners) {
            listener(chunk, 'stderr');
          }
        }
      };

      readStdout().catch(e => logger.error("[DockerHost] readStdout error: " + e));
      readStderr().catch(e => logger.error("[DockerHost] readStderr error: " + e));
    }

    const handle: HostProcessHandle = {
      pid: proc.pid || 0,
      get stdout() { return state.stdout; },
      get stderr() { return state.stderr; },
      get completed() { return state.completed; },
      get exitCode() { return state.exitCode; },

      write: (input: string) => {
        if (state.completed) return;
        if (usePty) {
          proc.terminal!.write(input);
        } else {
          try {
            (proc.stdin as any)!.write(input);
            (proc.stdin as any)!.flush();
          } catch (e) {
            logger.error("[DockerHost] write failed: " + e);
          }
        }
      },

      resize: (cols: number, rows: number) => {
        if (usePty) {
          proc.terminal!.resize(cols, rows);
        }
      },

      kill: async (sig: string) => {
        proc.kill(sig as any);
      },

      onOutput: (callback) => {
        listeners.add(callback);
      },

      wait: async () => {
        const exitCode = await proc.exited;
        const elapsed = Date.now() - startTime;

        state.completed = true;
        state.exitCode = exitCode;

        let finishReason = HostProcessFinishReason.Natural;
        if (exitCode === null) {
          finishReason = (options?.timeout && elapsed >= options.timeout)
            ? HostProcessFinishReason.Timeout
            : HostProcessFinishReason.Killed;
        }

        return {
          exitCode: exitCode ?? -1,
          stdout: state.stdout,
          stderr: state.stderr,
          elapsed,
          finishReason
        };
      }
    };

    proc.exited.then((code) => {
      state.completed = true;
      state.exitCode = code;
    });

    return handle;
  }

  async readFile(path: Bun.PathLike): Promise<string> {
    const res = await this.exec(`cat "${path}"`, { cwd: this.defaultCwd.toString() });
    if (res.exitCode !== 0) {
      throw new Error(`Failed to read ${path}: ${res.stderr}`);
    }
    return res.stdout;
  }

  async writeFile(path: Bun.PathLike, content: string): Promise<boolean> {
    const b64 = Buffer.from(content).toString('base64');
    const res = await this.exec(`echo "${b64}" | base64 -d > "${path}"`, { cwd: this.defaultCwd.toString() });
    return res.exitCode === 0;
  }

  async exists(path: Bun.PathLike): Promise<boolean> {
    const res = await this.exec(`test -e "${path}"`, { cwd: this.defaultCwd.toString() });
    return res.exitCode === 0;
  }

  async stat(path: Bun.PathLike): Promise<HostFileEntry> {
    const res = await this.exec(`stat -c "%n|%s|%Y|%a|%U|%G|%F" "${path}"`, { cwd: this.defaultCwd.toString() });
    if (res.exitCode !== 0) {
      throw new Error(`stat failed: ${res.stderr}`);
    }
    const parts = res.stdout.trim().split('|');
    return {
      name: parts[0] || '',
      path: path.toString(),
      isDirectory: parts[5] === 'directory',
      isSymlink: parts[5] === 'symbolic link',
      size: parseInt(parts[1] || '0', 10),
      mtime: parseInt(parts[2] || '0', 10) * 1000,
      mode: parseInt(parts[3] || '0', 10),
      owner: parts[4] || '',
      group: ''
    };
  }

  async listDir(path: Bun.PathLike, options?: HostListDirOptions): Promise<HostFileEntry[]> {
    const lsRes = await this.exec(`ls -la --time-style=+%s "${path}"`, { cwd: this.defaultCwd.toString() });
    if (lsRes.exitCode !== 0) {
      throw new Error(`Failed to list ${path}: ${lsRes.stderr}`);
    }

    const dirPath = path.toString();
    const excludeSet = new Set(options?.exclude || []);
    const result: HostFileEntry[] = [];

    const lines = lsRes.stdout.split('\n');
    for (let line of lines) {
      line = line.trim();
      if (!line || line.startsWith('total ')) continue;

      const parts = line.split(/\s+/);
      if (parts.length < 7) continue;

      const modeStr = parts[0]!;
      const owner = parts[2];
      const group = parts[3];
      const size = parseInt(parts[4]!, 10);
      const mtime = parseInt(parts[5]!, 10) * 1000;
      const name = parts.slice(6).join(' ');

      if (name === '.' || name === '..') continue;
      if (excludeSet.has(name)) continue;

      const isDirectory = modeStr.startsWith('d');
      const isSymlink = modeStr.startsWith('l');

      // Convert drwxr-xr-x to octal number
      let mode = 0;
      const mapping: Record<string, number> = { 'r': 4, 'w': 2, 'x': 1, '-': 0 };
      for (let i = 1; i < 10; i++) {
        const char = modeStr[i] || '-';
        const val = mapping[char] || 0;
        const pos = Math.floor((9 - i) / 3);
        mode += val * Math.pow(8, pos);
      }

      result.push({
        name,
        path: join(dirPath, name),
        isDirectory,
        isSymlink,
        size,
        mtime,
        mode,
        owner,
        group
      });
    }

    return result;
  }

  async remove(path: Bun.PathLike): Promise<boolean> {
    const res = await this.exec(`rm -rf "${path}"`, { cwd: this.defaultCwd.toString() });
    return res.exitCode === 0;
  }

  async mkdir(path: Bun.PathLike): Promise<boolean> {
    const res = await this.exec(`mkdir -p "${path}"`, { cwd: this.defaultCwd.toString() });
    return res.exitCode === 0;
  }

  private async execLocal(command: string): Promise<HostProcessResult> {
    const startTime = Date.now();
    logger.info(`[DockerHost] execLocal starting: ${command}`);
    
    try {
      const proc = Bun.spawn(['sh', '-c', command], {
        stdout: "pipe",
        stderr: "pipe"
      });
      
      const stdoutPromise = new Response(proc.stdout).text();
      const stderrPromise = new Response(proc.stderr).text();
      
      const exitCode = await proc.exited;
      const stdout = await stdoutPromise;
      const stderr = await stderrPromise;
      
      logger.info(`[DockerHost] execLocal finished: ${command} (code ${exitCode})`);
      
      return {
        exitCode: exitCode ?? -1,
        stdout,
        stderr,
        elapsed: Date.now() - startTime,
        finishReason: HostProcessFinishReason.Natural
      };
    } catch (e) {
      logger.error(`[DockerHost] execLocal FAILED: ${command} ` + e);
      throw e;
    }
  }


  private buildDockerArgs(command: string, options?: HostExecOptions): string[] {
    const cwd = options?.cwd?.toString() || this.defaultCwd.toString();
    const usePty = options?.pty !== false;
    const args = ['exec', '-i'];
    if (usePty) args.push('-t');
    args.push('-w', cwd);

    if (this.shouldRunAsRoot(command)) {
      args.push('-u', '0');
    }

    if (options?.env) {
      for (const [key, val] of Object.entries(options.env)) {
        args.push('-e', `${key}=${val}`);
      }
    }

    args.push(this.containerName, 'sh', '-c', command);

    return args;
  }

  private shouldRunAsRoot(command: string): boolean {
    return command.includes('apt-get') ||
      command.includes('apk add') ||
      command.includes('brew install');
  }

  private async runBootstrapScript(script: string): Promise<void> {
    if (!fs.existsSync(script)) {
      logger.warn(`[DockerHost] Bootstrap script not found: ${script}`);
      return;
    }

    const scriptContent = fs.readFileSync(script, 'utf-8');
    const res = await this.exec(scriptContent, { cwd: '/' });

    if (res.exitCode !== 0) {
      logger.warn(`[DockerHost] Bootstrap script failed: ${res.stderr}`);
    }
  }
}
