export enum HostProcessFinishReason {
  Natural, Timeout, Killed
}

export interface HostProcessResult {
  exitCode: number;
  stdout: string;
  stderr: string;
  elapsed: number;
  finishReason: HostProcessFinishReason;
}

export interface HostProcessHandle {
  pid: number;
  stdout: string;
  stderr: string;
  completed: boolean;
  exitCode: number | null;
  write(input: string): void;
  resize(cols: number, rows: number): void;
  kill(sig: string): Promise<void>;
  wait(): Promise<HostProcessResult>;
  onOutput(callback: (data: string, src: "stdout" | "stderr") => void): void;
}

export interface HostExecOptions {
  cwd: string | import("bun").PathLike;
  env?: Record<string, string>;
  timeout?: number;
  pty?: boolean;
}

export enum HostType {
  Local, Docker, RemoteSSH
}

export interface HostFileEntry {
  name: string;
  path: string;
  isDirectory: boolean;
  isSymlink: boolean;
  resolvedPath?: string;
  size: number;
  mtime: number;
  mode: number;
  owner?: string;
  group?: string;
}

export interface HostListDirOptions {
  exclude?: string[];
}

export interface IHost {
  type: HostType;
  defaultCwd: string | import("bun").PathLike;
  init(): Promise<void>;
  destroy(): Promise<void>;
  exec(command: string, options?: HostExecOptions): Promise<HostProcessResult>;
  spawn(command: string, options?: HostExecOptions): Promise<HostProcessHandle>;
  readFile(path: string): Promise<string>;
  writeFile(path: string, content: string): Promise<boolean>;
  exists(path: string): Promise<boolean>;
  stat(path: string): Promise<HostFileEntry>;
  listDir(path: string, options?: HostListDirOptions): Promise<HostFileEntry[]>;
  remove(path: string): Promise<boolean>;
  mkdir(path: string): Promise<boolean>;
}

export interface DockerHostOptions {
  image: string;
  containerName: string;
  volumes?: Record<string, string>;
  env?: Record<string, string>;
  bootstrapScript?: string;
  bootstrapScriptPath?: string;
  repo?: string;
}

export type HostConfig =
  | { type: HostType.Local }
  | { type: HostType.Docker; options: DockerHostOptions }
  | { type: HostType.RemoteSSH; options: import("ssh2").ConnectConfig & { privateKeyPath?: string } };

export interface DockerMount {
  hostPath: string;
  containerPath: string;
}
