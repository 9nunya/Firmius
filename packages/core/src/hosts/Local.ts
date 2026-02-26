import { join } from "node:path";
import { HostProcessFinishReason, HostType, type HostExecOptions, type HostFileEntry, type HostListDirOptions, type HostProcessHandle, type HostProcessResult, type IHost } from "@firmius/shared/types";
import { readdir, stat } from "node:fs/promises"
import { logger } from "@firmius/shared/utils";

export class LocalHost implements IHost {
  type = HostType.Local;
  defaultCwd: Bun.PathLike = process.cwd();

  constructor() { };

  init(): Promise<void> {
    return Promise.resolve();
  }

  destroy(): Promise<void> {
    return Promise.resolve();
  }

  async exec(command: string, options?: HostExecOptions): Promise<HostProcessResult> {
    const startTime = Date.now();

    const state = {
      stdout: '',
      stderr: '',
      completed: false,
      exitCode: null as number | null,
    };

    const proc = Bun.spawn(['sh', '-c', command], {
      cwd: options?.cwd?.toString() || this.defaultCwd.toString(),
      env: {
        ...process.env,
        ...options?.env,
        TERM: "xterm-256color"
      },
      terminal: {
        cols: 80,
        rows: 24,
        data(_terminal, data) {
          const chunk = new TextDecoder().decode(data);
          state.stdout += chunk;
        },
      },
      timeout: options?.timeout,
      killSignal: "SIGKILL"
    });

    const [exitCode, killSignal] = await Promise.all([
      proc.exited, proc.signalCode
    ]);

    const elapsed = Date.now() - startTime;

    return Promise.resolve({
      exitCode,
      stdout: state.stdout,
      stderr: '',
      elapsed,
      finishReason: exitCode !== null ? HostProcessFinishReason.Natural
        : options && options.timeout && elapsed == options.timeout && killSignal == "SIGKILL" ? HostProcessFinishReason.Timeout : HostProcessFinishReason.Killed
    })
  }


   async spawn(command: string, options?: HostExecOptions): Promise<HostProcessHandle> {
     const startTime = Date.now();
     const listeners = new Set<(data: string, source: "stdout" | "stderr") => void>();

     const state = {
       stdout: '',
       stderr: '',
       completed: false,
       exitCode: null as number | null,
       stdoutTask: null as Promise<void> | null,
       stderrTask: null as Promise<void> | null
     };

    const usePty = options?.pty !== false;

    const spawnOptions: any = {
      cwd: options?.cwd?.toString() || this.defaultCwd.toString(),
      env: {
        ...process.env,
        ...options?.env,
        TERM: "xterm-256color"
      },
      timeout: options?.timeout,
      killSignal: "SIGKILL"
    };

    if (usePty) {
      spawnOptions.terminal = {
        cols: 80,
        rows: 24,
        data(_terminal: any, data: Uint8Array) {
          const chunk = new TextDecoder().decode(data);
          state.stdout += chunk;
          for (const listener of listeners) {
            listener(chunk, "stdout");
          }
        },
      };
    } else {
      spawnOptions.stdout = "pipe";
      spawnOptions.stderr = "pipe";
      spawnOptions.stdin = "pipe";
    }

    const proc = Bun.spawn(['sh', '-c', command], spawnOptions);

    if (!usePty) {
      const readStdout = async () => {
        try {
          const stdout = proc.stdout as unknown as ReadableStream;
          const reader = stdout.getReader();
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            const chunk = new TextDecoder().decode(value as Uint8Array);
            state.stdout += chunk;
            for (const listener of listeners) {
              listener(chunk, "stdout");
            }
          }
        } catch (e) {
          logger.error("[LocalHost] readStdout error: " + e);
        }
      };
      const readStderr = async () => {
        try {
          const stderr = proc.stderr as unknown as ReadableStream;
          const reader = stderr.getReader();
          while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            const chunk = new TextDecoder().decode(value as Uint8Array);
            state.stderr += chunk;
            for (const listener of listeners) {
              listener(chunk, "stderr");
            }
          }
        } catch (e) {
          logger.error("[LocalHost] readStderr error: " + e);
        }
      };

      // Start reading tasks and keep references
      state.stdoutTask = readStdout();
      state.stderrTask = readStderr();
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
          (proc as any).terminal!.write(input);
        } else {
          try {
            (proc as any).stdin!.write(input);
            (proc as any).stdin!.flush();
          } catch (e) {
            logger.error("[LocalHost] write failed: " + e);
          }
        }
      },

      resize: (cols: number, rows: number) => {
        if (usePty) (proc as any).terminal!.resize(cols, rows);
      },

      kill: async (sig: string) => {
        proc.kill(sig as any);
      },

      onOutput: (callback) => {
        listeners.add(callback);
      },

      wait: async (): Promise<HostProcessResult> => {
        const exitCode = await proc.exited;
        const elapsed = Date.now() - startTime;

        // Wait for stdout/stderr readers to finish draining
        if (state.stdoutTask) await state.stdoutTask;
        if (state.stderrTask) await state.stderrTask;

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
    return await Bun.file(path.toString()).text();
  }

  async writeFile(path: Bun.PathLike, content: string): Promise<boolean> {
    return Promise.resolve(await Bun.write(path.toString(), content) > 0);
  }

  async exists(path: Bun.PathLike): Promise<boolean> {
    try {
      await stat(path.toString());
      return true;
    } catch {
      return false;
    }
  }

  async stat(path: Bun.PathLike): Promise<HostFileEntry> {
    const s = await stat(path.toString());
    const entry: HostFileEntry = {
      name: path.toString().split('/').pop() || '',
      path: path.toString(),
      isDirectory: s.isDirectory(),
      isSymlink: s.isSymbolicLink(),
      resolvedPath: undefined,
      size: s.size,
      mtime: s.mtimeMs,
      mode: s.mode,
      owner: s.uid.toString(),
      group: s.gid.toString()
    };
    return entry;
  }

  async listDir(path: Bun.PathLike, options?: HostListDirOptions): Promise<HostFileEntry[]> {
    try {
      const dirPath = path.toString();
      const entries = await readdir(dirPath, { withFileTypes: true });
      const excludeSet = new Set(options?.exclude || []);
      const result: HostFileEntry[] = [];

      for (const entry of entries) {
        if (excludeSet.has(entry.name)) continue;

        const entryPath = join(dirPath, entry.name);
        const s = await stat(entryPath);
        
        // Get owner/group names via child_process to be accurate on Linux/macOS
        let owner = s.uid.toString();
        let group = s.gid.toString();
        
        try {
          const ownerRes = Bun.spawnSync(["id", "-nu", s.uid.toString()]);
          if (ownerRes.success) owner = ownerRes.stdout.toString().trim();
          const groupRes = Bun.spawnSync(["id", "-ng", s.gid.toString()]);
          if (groupRes.success) group = groupRes.stdout.toString().trim();
        } catch (e) {
          // Fallback to IDs if id command fails
        }

        result.push({
          name: entry.name,
          path: entryPath,
          isDirectory: entry.isDirectory(),
          isSymlink: entry.isSymbolicLink(),
          resolvedPath: entry.isSymbolicLink() ? entryPath : undefined,
          size: s.size,
          mtime: s.mtimeMs,
          mode: s.mode,
          owner,
          group
        });
      }

      return result;
    } catch (error) {
      throw new Error(`Failed to list ${path}: ${error instanceof Error ? error.message : String(error)}`)
    }
  }

  async remove(path: Bun.PathLike): Promise<boolean> {
    const result = await this.exec(`rm -rf "${path}"`);
    return Promise.resolve(result.exitCode !== 0 ? false : true)
  }

  async mkdir(path: Bun.PathLike): Promise<boolean> {
    const result = await this.exec(`mkdir -p "${path}"`);
    return Promise.resolve(result.exitCode !== 0 ? false : true)
  }
}
