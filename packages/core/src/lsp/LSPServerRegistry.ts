import type { IHost } from "@firmius/shared/types";
import type { LSPDiagnostic } from "@firmius/shared/types";
import { logger } from "@firmius/shared/utils";
import path from "node:path";
import { LSPProtocol } from "./LSPProtocol";

const LOGGER_PREFIX = "[LSPServerRegistry]";

export interface LSPServerConfig {
  command: string;
  args: string[];
  env?: Record<string, string>;
}

interface OpenDocument {
  uri: string;
  version: number;
  content: string;
}

interface PendingDiagnosticPromise {
  resolve: (diagnostics: LSPDiagnostic[]) => void;
  reject: (error: Error) => void;
}

interface ServerAvailability {
  command: string;
  available: boolean;
}

const SERVER_CONFIGS: Record<string, LSPServerConfig> = {
  "ts": { command: "typescript-language-server", args: ["--stdio"] },
  "js": { command: "typescript-language-server", args: ["--stdio"] },
  "py": { command: "pyright-langserver", args: ["--stdio"] },
  "rs": { command: "rust-analyzer", args: [] },
  "cpp": { command: "clangd", args: [] },
  "c": { command: "clangd", args: [] },
  "go": { command: "gopls", args: ["serve", "-rpc.trace"], env: { "GOMEMLIMIT": "2GiB", "GOTOOLCHAIN": "local" } },
  "luau": { command: "luau-lsp", args: ["lsp"] },
  "lua": { command: "lua-language-server", args: [] },
};

const LANGUAGE_MAP: Record<string, string> = {
  "ts": "typescript",
  "js": "javascript",
  "py": "python",
  "rs": "rust",
  "cpp": "cpp",
  "c": "c",
  "go": "go",
  "luau": "luau",
  "lua": "lua"
};

const DEFAULT_DIAGNOSTIC_TIMEOUT_MS = 3000;
const SHUTDOWN_TIMEOUT_MS = 5000;
const REQUEST_TIMEOUT_MS = 20000;

export class LSPServerRegistry {
  private clients: Map<string, LSPProtocol> = new Map();
  private clientInitLocks: Map<string, Promise<LSPProtocol | undefined>> = new Map();
  private diagnostics: Map<string, LSPDiagnostic[]> = new Map();
  private openDocuments: Map<string, OpenDocument> = new Map();
  private pendingDiagnosticPromises: Map<string, PendingDiagnosticPromise[]> = new Map();
  private host: IHost;
  private rootUri: string;
  private serverConfigs: Record<string, LSPServerConfig>;

  constructor(host: IHost, rootUri: string, customServerConfigs?: Record<string, LSPServerConfig>) {
    this.host = host;
    this.rootUri = rootUri;
    this.serverConfigs = customServerConfigs ?? SERVER_CONFIGS;
  }

  async getClientForFile(filePath: string): Promise<LSPProtocol | undefined> {
    const ext = this.getFileExtension(filePath);
    const config = this.serverConfigs[ext];
    if (!config) {
      return undefined;
    }

    if (this.clients.has(ext)) {
      return this.clients.get(ext);
    }

    if (!this.clientInitLocks.has(ext)) {
      this.clientInitLocks.set(ext, this.initializeClient(ext));
    }

    try {
      return await this.clientInitLocks.get(ext);
    } finally {
      this.clientInitLocks.delete(ext);
    }
  }

  private async initializeClient(ext: string): Promise<LSPProtocol | undefined> {
    const config = this.serverConfigs[ext];
    if (!config) {
      return undefined;
    }

    try {
      const command = this.buildCommand(config.command, config.args);
      const handle = await this.host.spawn(command, {
        cwd: this.rootUri,
        pty: false,
        env: config.env
      });
      const client = new LSPProtocol(handle);

      handle.onOutput((_data: string, source: string) => {
        if (source === 'stderr') {
          // logger.error(`${LOGGER_PREFIX} [${ext} Server Error] ${data}`);
        }
      });

      client.onNotification("textDocument/publishDiagnostics", (params: { uri: string; diagnostics: LSPDiagnostic[] }) => {
        const cleanPath = params.uri.replace(/^file:\/\//, "");
        this.diagnostics.set(cleanPath, params.diagnostics);

        const pendingPromises = this.pendingDiagnosticPromises.get(cleanPath);
        if (pendingPromises) {
          for (const { resolve } of pendingPromises) {
            resolve(params.diagnostics);
          }
          this.pendingDiagnosticPromises.delete(cleanPath);
        }
      });

      const initParams = this.buildInitParams();

      await client.request("initialize", initParams, REQUEST_TIMEOUT_MS);
      client.notify("initialized", {});

      this.clients.set(ext, client);
      logger.info(`${LOGGER_PREFIX} Initialized LSP client for ${ext}`);
      return client;
    } catch (e) {
      const errorMessage = e instanceof Error ? e.message : String(e);
      logger.error(`${LOGGER_PREFIX} Failed to start LSP server for ${ext}: ${errorMessage}`);
      throw new Error(`Failed to start LSP server for ${ext}: ${errorMessage}`);
    }
  }

  private buildCommand(command: string, args: string[]): string {
    return `${command} ${args.join(" ")}`;
  }

  private buildInitParams(): any {
    return {
      processId: null,
      rootUri: `file://${this.rootUri}`,
      capabilities: {},
      workspaceFolders: [{ uri: `file://${this.rootUri}`, name: "workspace" }]
    };
  }

  private getFileExtension(filePath: string): string {
    return path.extname(filePath).slice(1);
  }

  getDiagnosticsForFile(filePath: string): LSPDiagnostic[] {
    return this.diagnostics.get(filePath) || [];
  }

  async shutdown(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Shutting down ${this.clients.size} LSP clients...`);
    const shutdownPromises: Promise<void>[] = [];

    for (const [ext, client] of this.clients.entries()) {
      shutdownPromises.push(
        (async () => {
          try {
            await client.request("shutdown", {}, REQUEST_TIMEOUT_MS);
            client.notify("exit", {});
          } catch {
            // Ignore shutdown errors - server might already be dead
          }

          try {
            await client.cleanup();
            logger.info(`${LOGGER_PREFIX} ${ext} client cleaned up successfully`);
          } catch (e) {
            const errorMessage = e instanceof Error ? e.message : String(e);
            logger.warn(`${LOGGER_PREFIX} ${ext} client cleanup failed: ${errorMessage}`);
          }
        })()
      );
    }

    await Promise.all(
      shutdownPromises.map(p =>
        Promise.race([
          p,
          new Promise<void>((_, reject) =>
            setTimeout(() => reject(new Error('Shutdown timeout')), SHUTDOWN_TIMEOUT_MS)
          )
        ]).catch(e => {
          const errorMessage = e instanceof Error ? e.message : String(e);
          logger.error(`${LOGGER_PREFIX} Shutdown failed: ${errorMessage}`);
        })
      )
    );

    this.clearAllMaps();
    logger.info(`${LOGGER_PREFIX} All LSP clients shut down and cleaned up`);
  }

  private clearAllMaps(): void {
    this.clients.clear();
    this.clientInitLocks.clear();
    this.openDocuments.clear();
    this.diagnostics.clear();
    this.pendingDiagnosticPromises.clear();
  }

  async killAll(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Emergency killing ${this.clients.size} LSP processes...`);

    for (const [ext, client] of this.clients.entries()) {
      try {
        await client.cleanup();
        logger.info(`${LOGGER_PREFIX} ${ext} process killed`);
      } catch (e) {
        const errorMessage = e instanceof Error ? e.message : String(e);
        logger.error(`${LOGGER_PREFIX} Failed to kill ${ext}: ${errorMessage}`);
      }
    }

    this.clients.clear();
    this.clientInitLocks.clear();
  }

  async getAvailability(): Promise<Record<string, ServerAvailability>> {
    const results: Record<string, ServerAvailability> = {};
    const extensions = Object.keys(this.serverConfigs);

    for (const ext of extensions) {
      const config = this.serverConfigs[ext]!;
      try {
        const res = await this.host.exec(`command -v ${config.command}`);
        results[ext] = {
          command: config.command,
          available: res.exitCode === 0
        };
      } catch {
        results[ext] = {
          command: config.command,
          available: false
        };
      }
    }

    return results;
  }

  async isAvailableForFile(filePath: string): Promise<{ available: boolean; reason?: string }> {
    const ext = this.getFileExtension(filePath);
    const config = this.serverConfigs[ext];
    if (!config) {
      return { available: false, reason: `No LSP server configured for extension: .${ext}` };
    }

    try {
      const res = await this.host.exec(`command -v ${config.command}`);
      if (res.exitCode !== 0) {
        return { available: false, reason: `LSP server binary '${config.command}' not found on host.` };
      }
      return { available: true };
    } catch (e) {
      return { available: false, reason: `Error checking availability: ${String(e)}` };
    }
  }

  async getAvailableLanguageExtensions(): Promise<string[]> {
    const extensions: string[] = [];
    for (const ext of Object.keys(this.serverConfigs)) {
      const availability = await this.isAvailableForFile(`dummy.${ext}`);
      if (availability.available) {
        extensions.push(ext);
      }
    }
    return extensions;
  }

  async openDocument(filePath: string, content: string): Promise<void> {
    const client = await this.getClientForFile(filePath);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Cannot open document: no LSP client for ${filePath}`);
      return;
    }

    const uri = `file://${filePath}`;
    const languageId = this.getLanguageId(filePath);

    this.openDocuments.set(filePath, {
      uri,
      version: 1,
      content
    });

    client.notify("textDocument/didOpen", {
      textDocument: {
        uri,
        languageId,
        version: 1,
        text: content
      }
    });
  }

  async notifyFileChanged(filePath: string, content: string): Promise<void> {
    const client = await this.getClientForFile(filePath);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Cannot notify file changed: no LSP client for ${filePath}`);
      return;
    }

    const uri = `file://${filePath}`;
    const doc = this.openDocuments.get(filePath);

    if (doc) {
      doc.version++;
      doc.content = content;

      client.notify("textDocument/didChange", {
        textDocument: { uri, version: doc.version },
        contentChanges: [{ text: content }]
      });
    } else {
      await this.openDocument(filePath, content);
    }
  }

  async waitForDiagnostics(filePath: string, timeoutMs: number = DEFAULT_DIAGNOSTIC_TIMEOUT_MS): Promise<LSPDiagnostic[]> {
    const existingDiagnostics = this.diagnostics.get(filePath);
    if (existingDiagnostics) {
      return existingDiagnostics;
    }

    return new Promise((resolve) => {
      const timer = setTimeout(() => {
        const pending = this.pendingDiagnosticPromises.get(filePath);
        if (pending) {
          const index = pending.findIndex(p => p.resolve === resolve);
          if (index !== -1) {
            pending.splice(index, 1);
          }
        }
        resolve(this.diagnostics.get(filePath) || []);
      }, timeoutMs);

      if (!this.pendingDiagnosticPromises.has(filePath)) {
        this.pendingDiagnosticPromises.set(filePath, []);
      }
      this.pendingDiagnosticPromises.get(filePath)!.push({
        resolve: (diagnostics) => {
          clearTimeout(timer);
          resolve(diagnostics);
        },
        reject: (error) => {
          clearTimeout(timer);
          logger.error(`${LOGGER_PREFIX} Diagnostics wait failed: ${error.message}`);
          resolve([]);
        }
      });
    });
  }

  private getLanguageId(filePath: string): string {
    const ext = this.getFileExtension(filePath);
    return LANGUAGE_MAP[ext] || ext;
  }
}
