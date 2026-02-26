import type { HostProcessHandle } from "@firmius/shared/types";
import type { LSPRequest, LSPNotification, LSPResponse } from "@firmius/shared/types";
import { logger } from "@firmius/shared/utils";

const DEFAULT_REQUEST_TIMEOUT_MS = 20000;
const STDERR_MAX_SIZE_BYTES = 10 * 1024;
const CLEANUP_GRACE_MS = 100;

export enum ConnectionState {
  Connecting = "connecting",
  Connected = "connected",
  Disconnected = "disconnected",
  Error = "error"
}

export class LSPProtocol {
  private handle: HostProcessHandle;
  private requestId = 0;
  private pendingRequests: Map<number | string, (res: LSPResponse) => void> = new Map();
  private buffer: Buffer = Buffer.alloc(0);
  private notificationCallbacks: Map<string, Array<(params: any) => void>> = new Map();
  private crashCallbacks: Array<(exitCode: number | null, reason: string) => void> = [];
  private state: ConnectionState = ConnectionState.Connecting;
  private stderrOutput: string = "";
  private lastActivity: number = Date.now();

  constructor(handle: HostProcessHandle) {
    this.handle = handle;
    this.setupOutputHandlers();
    this.setupCrashDetection();
  }

  private setupOutputHandlers(): void {
    this.handle.onOutput((data: string, source: "stdout" | "stderr") => {
      if (source === "stdout") {
        this.onData(data);
        this.updateActivity();
      } else if (source === "stderr") {
        this.handleStderr(data);
      }
    });
  }

  private setupCrashDetection(): void {
    this.handle.wait().then((result: any) => {
      if (this.state !== ConnectionState.Disconnected) {
        this.state = ConnectionState.Disconnected;
        const reason = this.stderrOutput || result.finishReason.toString();
        logger.error(`[LSPProtocol] LSP server process exited with code ${result.exitCode}`);
        this.notifyCrash(result.exitCode, reason);
      }
    }).catch((err: any) => {
      if (this.state !== ConnectionState.Disconnected) {
        this.state = ConnectionState.Error;
        logger.error(`[LSPProtocol] LSP server process wait() error: ${err}`);
        this.notifyCrash(null, err.message);
      }
    });
  }

  private handleStderr(data: string): void {
    this.stderrOutput += data;
    if (this.stderrOutput.length > STDERR_MAX_SIZE_BYTES) {
      this.stderrOutput = this.stderrOutput.slice(-STDERR_MAX_SIZE_BYTES);
    }
  }

  private updateActivity(): void {
    this.lastActivity = Date.now();
  }

  private notifyCrash(exitCode: number | null, reason: string): void {
    this.crashCallbacks.forEach((callback) => {
      try {
        callback(exitCode, reason);
      } catch (e) {
        logger.error(`[LSPProtocol] Error in crash callback: ${e}`);
      }
    });
  }

  getConnectionState(): ConnectionState {
    return this.state;
  }

  isConnected(): boolean {
    return this.state === ConnectionState.Connected;
  }

  getStderrOutput(): string {
    return this.stderrOutput;
  }

  getLastActivity(): number {
    return this.lastActivity;
  }

  private onData(data: string): void {
    this.buffer = Buffer.concat([this.buffer, Buffer.from(data)]);
    this.processBuffer();
  }

  private processBuffer(): void {
    while (true) {
      const bufferStr = this.buffer.toString("utf-8");
      const headerMatch = bufferStr.match(/Content-Length: (\d+)\r\n\r\n/);
      if (!headerMatch) break;

      const contentLength = parseInt(headerMatch[1]!, 10);
      const headerStr = headerMatch[0];
      const headerIndex = headerMatch.index!;
      const bodyStartIndex = headerIndex + Buffer.from(headerStr).length;

      if (this.buffer.length < bodyStartIndex + contentLength) {
        break;
      }

      if (headerIndex > 0) {
        this.buffer = this.buffer.subarray(headerIndex);
        continue;
      }

      const rawContent = this.buffer.subarray(bodyStartIndex, bodyStartIndex + contentLength);
      this.buffer = this.buffer.subarray(bodyStartIndex + contentLength);

      try {
        const message = JSON.parse(rawContent.toString("utf-8"));
        this.handleMessage(message);
      } catch (e) {
        logger.error(`[LSPProtocol] Failed to parse LSP message: ${e}`);
      }
    }
  }

  private handleMessage(message: any): void {
    if (("id" in message) && (("result" in message) || ("error" in message))) {
      const callback = this.pendingRequests.get(message.id);
      if (callback) {
        callback(message as LSPResponse);
        this.pendingRequests.delete(message.id);

        if (message.result && message.result.capabilities !== undefined) {
          this.state = ConnectionState.Connected;
          logger.info("[LSPProtocol] LSP client connected successfully");
        }
      }
    } else if ("method" in message) {
      const callbacks = this.notificationCallbacks.get(message.method);
      if (callbacks) {
        callbacks.forEach((cb) => cb(message.params));
      }
    }
  }

  setConnected(): void {
    this.state = ConnectionState.Connected;
  }

  setDisconnected(): void {
    this.state = ConnectionState.Disconnected;
  }

  async isAlive(): Promise<boolean> {
    if (this.state === ConnectionState.Disconnected || this.state === ConnectionState.Error) {
      return false;
    }

    if (this.handle.completed) {
      return false;
    }

    try {
      await this.request("$/invokeCallback", {}, 500);
    } catch (e) {
      if (e instanceof Error && (e.message.includes("timed out") || e.message.includes("connection"))) {
        return false;
      }
    }

    return true;
  }

  onCrash(callback: (exitCode: number | null, reason: string) => void): () => void {
    this.crashCallbacks.push(callback);

    return () => {
      const index = this.crashCallbacks.indexOf(callback);
      if (index !== -1) {
        this.crashCallbacks.splice(index, 1);
      }
    };
  }

  async request(method: string, params: any, timeoutMs: number = DEFAULT_REQUEST_TIMEOUT_MS): Promise<any> {
    if (this.state === ConnectionState.Disconnected) {
      throw new Error(`[LSPProtocol] Request '${method}' failed: Server disconnected`);
    }

    const id = this.requestId++;
    const request: LSPRequest = {
      jsonrpc: "2.0",
      id,
      method,
      params
    };

    const json = JSON.stringify(request);
    const body = Buffer.from(json);

    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        if (this.pendingRequests.has(id)) {
          this.pendingRequests.delete(id);
          reject(new Error(`[LSPProtocol] Request '${method}' (id: ${id}) timed out after ${timeoutMs}ms`));
        }
      }, timeoutMs);

      this.pendingRequests.set(id, (res) => {
        clearTimeout(timeout);
        if (res.error) {
          reject(new Error(`[LSPProtocol] Error (${res.error.code}): ${res.error.message}`));
        } else {
          resolve(res.result);
        }
      });

      try {
        this.handle.write(`Content-Length: ${body.length}\r\n\r\n${json}`);
        this.updateActivity();
      } catch (e) {
        clearTimeout(timeout);
        this.pendingRequests.delete(id);
        reject(e);
      }
    });
  }

  notify(method: string, params: any): void {
    if (this.state === ConnectionState.Disconnected) {
      logger.warn(`[LSPProtocol] Notify '${method}' skipped: Server disconnected`);
      return;
    }

    const notification: LSPNotification = {
      jsonrpc: "2.0",
      method,
      params
    };
    const json = JSON.stringify(notification);
    const body = Buffer.from(json);
    try {
      this.handle.write(`Content-Length: ${body.length}\r\n\r\n${json}`);
      this.updateActivity();
    } catch (e) {
      const errorMessage = e instanceof Error ? e.message : String(e);
      logger.error(`[LSPProtocol] Notify '${method}' failed: ${errorMessage}`);
    }
  }

  onNotification(method: string, callback: (params: any) => void): () => void {
    const callbacks = this.notificationCallbacks.get(method) || [];
    this.notificationCallbacks.set(method, [...callbacks, callback]);

    return () => {
      const current = this.notificationCallbacks.get(method) || [];
      this.notificationCallbacks.set(method, current.filter(cb => cb !== callback));
    };
  }

  async cleanup(): Promise<void> {
    this.state = ConnectionState.Disconnected;

    const pendingErrors = new Map(this.pendingRequests);
    this.pendingRequests.clear();

    for (const [id, callback] of pendingErrors) {
      try {
        callback({ jsonrpc: "2.0", id, error: { code: -32600, message: "Connection closed" } });
      } catch (e) {
        // Ignore callback errors
      }
    }

    this.crashCallbacks = [];
    this.notificationCallbacks.clear();

    if (!this.handle.completed) {
      try {
        await this.handle.kill("SIGTERM");
        await new Promise((resolve) => setTimeout(resolve, CLEANUP_GRACE_MS));

        if (!this.handle.completed) {
          await this.handle.kill("SIGKILL");
        }
      } catch (e) {
        // Process might already be dead
      }
    }
  }
}
