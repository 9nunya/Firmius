
import { Client, type ConnectConfig } from "ssh2";
import { type IHost, HostType, type HostExecOptions, type HostProcessResult, type HostProcessHandle, HostProcessFinishReason, type HostListDirOptions, type HostFileEntry } from "@firmius/shared/types";
import type { PathLike } from "bun";

export class RemoteSSHHost implements IHost {
  type = HostType.RemoteSSH;
  defaultCwd: PathLike = "/tmp";

  private client: Client;
  private config: ConnectConfig;
  private connected: boolean = false;

  constructor(config: ConnectConfig, defaultCwd?: string) {
    this.client = new Client();
    this.config = config;
    if (defaultCwd) this.defaultCwd = defaultCwd;
  }

  async init(): Promise<void> {
    if (this.connected) return;

    return new Promise((resolve, reject) => {
      this.client.on('ready', () => {
        this.connected = true;
        resolve();
      }).on('error', (err) => {
        reject(err);
      }).connect(this.config);
    });
  }

  async destroy(): Promise<void> {
    this.client.end();
    this.connected = false;
  }

  async exec(command: string, options?: HostExecOptions): Promise<HostProcessResult> {
    if (!this.connected) await this.init();

    const cmd = this.buildCmd(command, options);
    const startTime = Date.now();

    return new Promise((resolve, reject) => {
      this.client.exec(cmd, (err, stream) => {
        if (err) return reject(err);

        let stdout = "";
        let stderr = "";

        stream.on('close', (code: number) => {
          const elapsed = Date.now() - startTime;
          resolve({
            exitCode: code,
            stdout,
            stderr,
            elapsed,
            finishReason: HostProcessFinishReason.Natural
          });
        }).on('data', (data: Buffer) => {
          stdout += data.toString();
        }).stderr.on('data', (data: Buffer) => {
          stderr += data.toString();
        });
      });
    });
  }

  async spawn(command: string, options?: HostExecOptions): Promise<HostProcessHandle> {
    if (!this.connected) await this.init();

    const cmd = this.buildCmd(command, options);
    const startTime = Date.now();
    const listeners = new Set<(data: string, source: "stdout" | "stderr") => void>();
    const state = { stdout: "", stderr: "", completed: false, exitCode: null as number | null };

    return new Promise((resolve, reject) => {
      const usePty = options?.pty !== false;
      this.client.exec(cmd, { pty: usePty ? { term: 'xterm-256color' } : undefined }, (err, stream) => {
        if (err) return reject(err);

        stream.on('data', (data: Buffer) => {
          const str = data.toString();
          state.stdout += str;
          listeners.forEach(l => l(str, "stdout"));
        });

        stream.stderr.on('data', (data: Buffer) => {
          const str = data.toString();
          state.stderr += str;
          listeners.forEach(l => l(str, "stderr"));
        });

        stream.on('close', (code: number) => {
          state.completed = true;
          state.exitCode = code;
        });

        const handle: HostProcessHandle = {
          pid: 0,
          get stdout() { return state.stdout; },
          get stderr() { return state.stderr; },
          get completed() { return state.completed; },
          get exitCode() { return state.exitCode; },

          write: (input: string) => {
            stream.write(input);
          },

          resize: (cols: number, rows: number) => {
            stream.setWindow(rows, cols, 0, 0);
          },

          kill: async (sig: string) => {
            stream.signal(sig);
            stream.close();
          },

          onOutput: (cb) => listeners.add(cb),

          wait: async (): Promise<HostProcessResult> => {
            if (state.completed) {
              return {
                exitCode: state.exitCode || 0,
                stdout: state.stdout,
                stderr: state.stderr,
                elapsed: Date.now() - startTime,
                finishReason: HostProcessFinishReason.Natural
              };
            }

            return new Promise(r => {
              stream.on('close', (code: number) => {
                r({
                  exitCode: code,
                  stdout: state.stdout,
                  stderr: state.stderr,
                  elapsed: Date.now() - startTime,
                  finishReason: HostProcessFinishReason.Natural
                });
              });
            });
          }
        };
        resolve(handle);
      });
    });
  }

  private buildCmd(command: string, options?: HostExecOptions) {
    let prefix = "export PATH=$PATH:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin; ";
    const cwd = options?.cwd || this.defaultCwd;
    if (cwd) prefix += `cd ${cwd} && `;
    if (options?.env) {
      for (const [k, v] of Object.entries(options.env)) {
        prefix += `export ${k}="${v}"; `;
      }
    }
    return prefix + command;
  }

  async readFile(path: PathLike): Promise<string> {
    const res = await this.exec(`cat "${path}"`);
    if (res.exitCode !== 0) throw new Error(`readFile failed: ${res.stderr}`);
    return res.stdout;
  }

  async writeFile(path: PathLike, content: string): Promise<boolean> {
    const b64 = Buffer.from(content).toString('base64');
    const res = await this.exec(`echo "${b64}" | base64 -d > "${path}"`);
    return res.exitCode === 0;
  }

  async exists(path: PathLike): Promise<boolean> {
    const res = await this.exec(`test -e "${path}"`);
    return res.exitCode === 0;
  }

  async stat(path: PathLike): Promise<HostFileEntry> {
    const res = await this.exec(`stat -c "%n|%s|%Y|%a|%U|%G|%F" "${path}"`);
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

  async listDir(path: PathLike, options?: HostListDirOptions): Promise<HostFileEntry[]> {
    const res = await this.exec(`ls -la --time-style=+%s "${path}"`);
    if (res.exitCode !== 0) throw new Error(`listDir failed: ${res.stderr}`);

    const dirPath = path.toString();
    const lines = res.stdout.split('\n').filter(Boolean);
    const exclude = new Set(options?.exclude || []);
    const result: HostFileEntry[] = [];

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
      if (exclude.has(name)) continue;

      result.push({
        name,
        path: `${dirPath}/${name}`,
        isDirectory: modeStr.startsWith('d'),
        isSymlink: modeStr.startsWith('l'),
        size,
        mtime,
        mode: 0,
        owner,
        group
      });
    }
    return result;
  }

  async remove(path: PathLike): Promise<boolean> {
    const res = await this.exec(`rm -rf "${path}"`);
    return res.exitCode === 0;
  }

  async mkdir(path: PathLike): Promise<boolean> {
    const res = await this.exec(`mkdir -p "${path}"`);
    return res.exitCode === 0;
  }
}
