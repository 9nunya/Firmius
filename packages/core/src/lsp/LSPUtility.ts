import { LSPProtocol } from './LSPProtocol';
import { LSPServerRegistry, type LSPServerConfig } from './LSPServerRegistry';
import { SymbolIndexer, type SymbolInfo, SymbolKindNames } from './SymbolIndexer';
import { ReferenceGraph, type TopFileResult, type ReferenceSummaryEntry } from './ReferenceGraph';
import { SemanticOperations } from './SemanticOperations';
import { FileScanner, type EntryPoint } from './FileScanner';
import { LanguageDetector } from './LanguageDetector';
import { LanguageHandler } from './LanguageHandler';

import type { LSPLocation, DocumentSymbol, Hover, CallHierarchyNode, ReferencesGraph, LSPDiagnostic } from '@firmius/shared/types';
import { SymbolKind } from '@firmius/shared/types';
import { logger, LRUCache, DebouncedNotifier } from '@firmius/shared/utils';
import type { IHost } from '@firmius/shared/types';

// ============================================================================
// CONSTANTS
// ============================================================================


const DEFAULT_DEBOUNCE_DELAY = 300;
const DEFAULT_SYMBOL_LIMIT = 15000;
const LOG_PREFIX = '[LSPUtility]';

// ============================================================================
// INTERFACES
// ============================================================================

export interface StructureSummary {
  languageCounts: Record<string, number>;
  totalFiles: number;
  languages: string[];
}

export interface CodebaseStats {
  languages: Map<string, number>;
  totalFiles: number;
  entryPoints: EntryPoint[];
  topFiles: TopFileResult[];
  structureSummary: StructureSummary;
  lastScanned: number;
}

export interface LSPUtilityOptions {
  symbolLimit?: number;
  debounceDelay?: number;
  customServerConfigs?: Record<string, LSPServerConfig>;
}

// Symbol map interface for ReferenceGraph
interface SymbolMap {
  entries(): IterableIterator<[string, SymbolInfo[]]>;
  get(key: string): SymbolInfo[] | undefined;
  values(): IterableIterator<SymbolInfo[]>;
}

// LSP Metrics interface
export interface LSPMetrics {
  scanDurationMs: number;
  filesProcessed: number;
  symbolsIndexed: number;
  languagesDetected: number;
  edgesBuilt: number;
}

// ============================================================================
// LSP UTILITY CLASS - Main Facade
// ============================================================================

/**
 * LSPUtility is the main facade class that combines all internal LSP modules.
 * It provides a unified API for:
 * - LSP client management (from LSPManager)
 * - Codebase intelligence (from CodebaseIntelligence)
 * - Semantic operations
 * - File scanning and language detection
 * - Language-specific setup (Go, Luau, etc.)
 */
export class LSPUtility {
  // Internal module instances
  private protocol: LSPProtocol | null = null;
  private serverRegistry: LSPServerRegistry;
  private symbolIndexer: SymbolIndexer;
  private referenceGraph: ReferenceGraph;
  private semanticOps: SemanticOperations;
  private fileScanner: FileScanner;
  private languageDetector: LanguageDetector;
  private languageHandler: LanguageHandler;

  // Host and configuration
  private host: IHost;
  private rootUri: string;
  private options: LSPUtilityOptions;

  // Caching and state
  private cache: LRUCache<any>;
  private changeNotifier: DebouncedNotifier;
  private lastScanTime: number = 0;
  private scanComplete: boolean = false;
  private readonly CACHE_TTL_MS: number = 5 * 60 * 1000;
  private isScanning: boolean = false;
  private codebaseStats: CodebaseStats | null = null;
  private scanStartTime: number = 0;
  private scanDurationMs: number = 0;
  private allFilesScored: TopFileResult[] = [];

  constructor(host: IHost, rootUri: string, options?: LSPUtilityOptions) {
    this.host = host;
    this.rootUri = rootUri;
    this.options = options ?? {};

    logger.info(`${LOG_PREFIX} Initializing LSPUtility for root: ${rootUri}`);

    // Initialize cache
    this.cache = new LRUCache<any>(this.options.symbolLimit ?? DEFAULT_SYMBOL_LIMIT);

    // Initialize debounced notifier
    this.changeNotifier = new DebouncedNotifier(this.options.debounceDelay ?? DEFAULT_DEBOUNCE_DELAY);
    this.changeNotifier.onChange((files) => {
      logger.info(`${LOG_PREFIX} Files changed: ${files.length} files`);
      this.handleFilesChanged(files);
    });

    // Initialize LSPServerRegistry
    this.serverRegistry = new LSPServerRegistry(host, rootUri, this.options.customServerConfigs);

    // Initialize FileScanner
    this.fileScanner = new FileScanner(
      {
        readFile: async (path: string) => this.host.readFile(path),
        exec: async (command: string, options?: any) => {
          const result = await this.host.exec(command, options);
          return { stdout: result.stdout };
        },
        findFilesByExtension: async (ext: string, limit?: number) => {
          const result = await this.host.exec(
            `find "${rootUri}" -type f -name "*.${ext}" 2>/dev/null | head -n ${limit ?? 100}`,
            { cwd: rootUri }
          );
          return result.stdout.trim().split('\n').filter(f => f);
        }
      },
      {
        getAvailableLanguageExtensions: () => this.getAvailableLanguageExtensions()
      },
      rootUri
    );

    // Initialize LanguageDetector
    this.languageDetector = new LanguageDetector(
      {
        readFile: async (path: string) => this.host.readFile(path),
        exec: async (command: string, options?: any) => {
          const result = await this.host.exec(command, options);
          return { stdout: result.stdout };
        }
      },
      {
        getAvailableLanguageExtensions: () => this.getAvailableLanguageExtensions()
      },
      rootUri
    );

    // Initialize LanguageHandler
    this.languageHandler = new LanguageHandler(
      {
        exec: async (command: string, options?: any) => {
          const result = await this.host.exec(command, options);
          return { stdout: result.stdout, exitCode: result.exitCode };
        },
        readFile: async (path: string) => this.host.readFile(path),
        writeFile: async (path: string, content: string) => {
          await this.host.writeFile(path, content);
        }
      },
      {
        getClientForFile: async (filePath: string) => this.getClientForFile(filePath),
        openDocument: async (filePath: string, content: string) => this.openDocument(filePath, content),
        warmupLanguage: async (language: string) => this.warmupLanguage(language)
      },
      rootUri
    );

    // Initialize SymbolIndexer with registry and host adapters
    this.symbolIndexer = new SymbolIndexer(
      {
        getClientForFile: async (filePath: string) => {
          const client = await this.getClientForFile(filePath);
          if (!client) return undefined;
          return {
            request: async (method: string, params: any, timeout?: number) =>
              client.request(method, params, timeout)
          };
        },
        openDocument: async (filePath: string, content: string) =>
          this.openDocument(filePath, content)
      },
      {
        readFile: async (path: string) => this.host.readFile(path)
      },
      { symbolLimit: this.options.symbolLimit }
    );

    // Initialize ReferenceGraph with a SymbolMap adapter
    const symbolMapAdapter: SymbolMap = {
      entries: () => this.symbolIndexer.getSymbolEntries()[Symbol.iterator](),
      get: (key: string) => this.symbolIndexer.getSymbolInfo(key),
      values: function* () {
        const entries = this.entries();
        for (const [, value] of entries) {
          yield value;
        }
      }
    };

    this.referenceGraph = new ReferenceGraph(
      symbolMapAdapter,
      {
        getClientForFile: async (filePath: string) => {
          const client = await this.getClientForFile(filePath);
          if (!client) return null;
          return {
            request: async (method: string, params: any, timeout?: number) =>
              client.request(method, params, timeout)
          };
        },
        openDocument: async (filePath: string, content: string) =>
          this.openDocument(filePath, content)
      },
      {
        readFile: async (path: string) => this.host.readFile(path)
      }
    );

    // Initialize SemanticOperations
    this.semanticOps = new SemanticOperations(
      {
        getClientForFile: async (filePath: string) => {
          const client = await this.getClientForFile(filePath);
          if (!client) return undefined;
          return {
            request: async (method: string, params: any, timeout?: number) =>
              client.request(method, params, timeout)
          };
        },
        openDocument: async (filePath: string, content: string) =>
          this.openDocument(filePath, content)
      },
      {
        readFile: async (path: string) => this.host.readFile(path)
      }
    );

    logger.info(`${LOG_PREFIX} LSPUtility initialized successfully`);
  }

  // ============================================================================
  // LSP CLIENT MANAGEMENT (From LSPManager)
  // ============================================================================

  /**
   * Get or create an LSP client for a specific file
   */
  async getClientForFile(filePath: string): Promise<LSPProtocol | undefined> {
    return this.serverRegistry.getClientForFile(filePath);
  }

  /**
   * Open a document in the appropriate LSP server
   */
  async openDocument(filePath: string, content: string): Promise<void> {
    await this.serverRegistry.openDocument(filePath, content);
    logger.debug(`${LOG_PREFIX} Opened document: ${filePath}`);
  }

  /**
   * Prewarm the LSP for a specific language by opening key files
   */
  async warmupLanguage(language: string): Promise<void> {
    logger.info(`${LOG_PREFIX} Warming up language: ${language}`);
    await this.languageHandler.prewarmLanguage(language);
  }

   /**
    * Get diagnostics for a file
    */
   async getDiagnostics(filePath: string): Promise<LSPDiagnostic[]> {
     return this.serverRegistry.getDiagnosticsForFile(filePath);
   }

   /**
    * Get diagnostics for a file (alias for getDiagnostics)
    */
   async getDiagnosticsForFile(filePath: string): Promise<LSPDiagnostic[]> {
     return this.getDiagnostics(filePath);
   }

  /**
   * Get list of available language extensions based on installed LSP servers
   */
  getAvailableLanguageExtensions(): string[] {
    // Note: This is synchronous but the registry method is async
    // We'll return a cached value or compute it
    return ['ts', 'js', 'py', 'rs', 'cpp', 'c', 'go', 'luau', 'lua'];
  }

  // ============================================================================
  // CODEBASE INTELLIGENCE (From CodebaseIntelligence)
  // ============================================================================

  /**
   * Scan the codebase to build symbol index and reference graph
   */
  async scan(forceRefresh?: boolean): Promise<CodebaseStats> {
    if (!forceRefresh && this.scanComplete && (Date.now() - this.lastScanTime < this.CACHE_TTL_MS)) {
      logger.info(`${LOG_PREFIX} scan(): returning cached codebase stats (age=${Date.now() - this.lastScanTime}ms)`);
      return this.codebaseStats!;
    }

    if (this.isScanning) {
      logger.warn(`${LOG_PREFIX} Scan already in progress, waiting...`);
      while (this.isScanning) {
        await new Promise(resolve => setTimeout(resolve, 100));
      }
      if (this.codebaseStats && !forceRefresh) {
        return this.codebaseStats;
      }
    }

    this.isScanning = true;
    this.scanStartTime = Date.now();
    logger.info(`${LOG_PREFIX} Starting codebase scan...`);

    try {
      // Clear existing data if force refresh
      if (forceRefresh) {
        this.clear();
      }

      // Detect languages
      const languages = await this.languageDetector.detectLanguages();
      logger.info(`${LOG_PREFIX} Detected languages: ${languages.join(', ')}`);

      // Luau-specific setup
      if (languages.includes('luau') || languages.includes('lua')) {
        const isLuauProject = await this.languageDetector.isLuauProject();
        if (isLuauProject) {
          logger.info(`${LOG_PREFIX} Setting up Luau workspace...`);
          await this.languageHandler.setupLuauWorkspace();
          this.fileScanner.setLuauProject(true);
        }
      }

      // Scan files
      const fileStats = await this.fileScanner.scan(languages);
      logger.info(`${LOG_PREFIX} Scanned ${fileStats.totalFiles} files`);

      // Build symbol index for each language
      const languageCounts = new Map<string, number>();
      for (const language of languages) {
        const files = await this.findFilesForLanguage(language);
        languageCounts.set(language, files.length);

        for (const file of files) {
          try {
            const result = await this.symbolIndexer.processFileForSymbols(file, language);
            for (const symbol of result.symbols) {
              this.symbolIndexer.addSymbol(symbol);
              this.referenceGraph.addSymbol(symbol);
            }
          } catch (e) {
            logger.debug(`${LOG_PREFIX} Error processing ${file}: ${e}`);
          }
        }
      }

      // Build reference graph
      await this.referenceGraph.buildReferenceGraph();

      // Compute scan duration
      this.scanDurationMs = Date.now() - this.scanStartTime;

      // Compute full scored files list with entry point boost
      const rawTop = this.referenceGraph.getTopFiles(Number.MAX_SAFE_INTEGER);
      const entryPointSet = new Set(fileStats.entryPoints.map(ep => ep.path));
      const allScored: TopFileResult[] = rawTop.map(f => ({
        ...f,
        score: entryPointSet.has(f.path) ? f.score + 1000 : f.score
      }));
      allScored.sort((a, b) => b.score - a.score);
      this.allFilesScored = allScored;

      // Get top files for immediate stats (top 20)
      const topFiles = this.allFilesScored.slice(0, 20);

      // Create structure summary
      const structureSummary: StructureSummary = {
        languageCounts: fileStats.languageCounts,
        totalFiles: fileStats.totalFiles,
        languages: fileStats.languages
      };

      // Create codebase stats
      this.codebaseStats = {
        languages: languageCounts,
        totalFiles: fileStats.totalFiles,
        entryPoints: fileStats.entryPoints,
        topFiles,
        structureSummary,
        lastScanned: Date.now()
      };

      this.lastScanTime = Date.now();
      this.scanComplete = true;
      logger.info(`${LOG_PREFIX} Scan complete: ${fileStats.totalFiles} files, ${this.symbolIndexer.getSymbolCount()} symbols indexed (${this.scanDurationMs}ms)`);

      return this.codebaseStats;
    } catch (error) {
      logger.error(`${LOG_PREFIX} Scan failed: ${error}`);
      throw error;
    } finally {
      this.isScanning = false;
    }
  }

  /**
   * Get information about a specific symbol
   */
  getSymbolInfo(symbolName: string): SymbolInfo[] {
    return this.symbolIndexer.getSymbolInfo(symbolName);
  }

  /**
   * Get top symbols by usage/reference count
   */
  getTopSymbols(limit?: number, kinds?: SymbolKind[]): Array<{ name: string; kind: string; file: string; line: number; character: number; callerCount: number }> {
    return this.symbolIndexer.getTopSymbols(limit, kinds);
  }

  /**
   * Get files that are referenced by a given file
   */
  getFileReferences(filePath: string): Set<string> {
    return this.referenceGraph.getFileReferences(filePath);
  }

  /**
   * Get files that reference a given file
   */
  getFileReferencedBy(filePath: string): Set<string> {
    return this.referenceGraph.getFileReferencedBy(filePath);
  }

  // ============================================================================
  // SEMANTIC OPERATIONS
  // ============================================================================

  /**
   * Get definition locations for a symbol at the given position
   */
  async getDefinition(path: string, line: number, character: number): Promise<LSPLocation[]> {
    logger.debug(`${LOG_PREFIX} Getting definition at ${path}:${line}:${character}`);
    return this.semanticOps.getDefinition(path, line, character);
  }

  /**
   * Get hover information for a symbol at the given position
   */
  async getHover(path: string, line: number, character: number): Promise<Hover | null> {
    logger.debug(`${LOG_PREFIX} Getting hover at ${path}:${line}:${character}`);
    return this.semanticOps.getHover(path, line, character);
  }

  /**
   * Get all references to a symbol at the given position
   */
  async getReferences(path: string, line: number, character: number): Promise<LSPLocation[]> {
    logger.debug(`${LOG_PREFIX} Getting references at ${path}:${line}:${character}`);
    return this.semanticOps.getReferences(path, line, character);
  }

  /**
   * Get document symbols for a file
   */
  async getDocumentSymbols(path: string, kinds?: SymbolKind[]): Promise<DocumentSymbol[]> {
    logger.debug(`${LOG_PREFIX} Getting document symbols for ${path}`);
    return this.semanticOps.getDocumentSymbols(path, kinds);
  }

  /**
   * Get call hierarchy for a symbol
   */
  async getCallHierarchy(
    path: string,
    line: number,
    character: number,
    direction: 'incoming' | 'outgoing',
    maxDepth: number = 2
  ): Promise<CallHierarchyNode[]> {
    logger.debug(`${LOG_PREFIX} Getting call hierarchy at ${path}:${line}:${character}`);
    return this.semanticOps.getCallHierarchy(path, line, character, direction, maxDepth);
  }

  /**
   * Get references graph for a symbol
   */
  async getReferencesGraph(
    path: string,
    line: number,
    character: number,
    maxDepth: number = 1
  ): Promise<ReferencesGraph> {
    logger.debug(`${LOG_PREFIX} Getting references graph at ${path}:${line}:${character}`);
    return this.semanticOps.getReferencesGraph(path, line, character, maxDepth);
  }

  // ============================================================================
  // LIFECYCLE
  // ============================================================================

  /**
   * Clear all cached data
   */
   clear(): void {
     logger.info(`${LOG_PREFIX} Clearing all cached data`);
     this.symbolIndexer.clear();
     this.referenceGraph.clear();
     this.cache.clear();
      this.codebaseStats = null;
      this.lastScanTime = 0;
      this.scanComplete = false;
      this.allFilesScored = [];
    }

  /**
   * Dispose of all resources
   */
  async dispose(): Promise<void> {
    logger.info(`${LOG_PREFIX} Disposing LSPUtility...`);

    this.changeNotifier.dispose();
    this.clear();

    await this.serverRegistry.shutdown();

    if (this.protocol) {
      await this.protocol.cleanup();
    }

    logger.info(`${LOG_PREFIX} LSPUtility disposed`);
  }

  // ============================================================================
  // UTILITY METHODS
  // ============================================================================

  /**
   * Check if the codebase needs to be rescanned
   */
  isStale(): boolean {
    if (!this.lastScanTime) return true;
    return (Date.now() - this.lastScanTime) > this.CACHE_TTL_MS;
  }

  /**
   * Get the last scan time
   */
  getLastScanTime(): number {
    return this.lastScanTime;
  }

  /**
   * Get current codebase stats
   */
  getCodebaseStats(): CodebaseStats | null {
    return this.codebaseStats;
  }

  /**
   * Check if a scan is in progress
   */
  getIsScanning(): boolean {
    return this.isScanning;
  }

  /**
   * Get symbol count
   */
  getSymbolCount(): number {
    return this.symbolIndexer.getSymbolCount();
  }

  // ============================================================================
  // LSP AVAILABILITY (from LSPManager)
  // ============================================================================

   /**
    * Get availability for each language's LSP server
    * Returns map with command and availability: { ts: { command: "typescript-language-server", available: true }, ... }
    */
   async getAvailability(): Promise<Record<string, { command: string; available: boolean }>> {
     return this.serverRegistry.getAvailability();
   }

   /**
    * Check if LSP is available for a specific file
    */
   async isAvailableForFile(filePath: string): Promise<{ available: boolean; reason?: string }> {
     return this.serverRegistry.isAvailableForFile(filePath);
   }

  /**
   * Notify LSP server of file content change
   */
  async notifyFileChanged(filePath: string, content: string): Promise<void> {
    await this.serverRegistry.notifyFileChanged(filePath, content);
  }

  /**
   * Wait for diagnostics to be ready for a file
   */
  async waitForDiagnostics(filePath: string, timeoutMs?: number): Promise<LSPDiagnostic[]> {
    return this.serverRegistry.waitForDiagnostics(filePath, timeoutMs);
  }

  // ============================================================================
  // ANALYSIS RESULTS (from CodebaseIntelligence)
  // ============================================================================

  /**
   * Get top files by importance/symbol count
   */
   getTopFiles(limit: number = 20, languages?: string[]): TopFileResult[] {
     if (!this.allFilesScored || this.allFilesScored.length === 0) {
       return [];
     }
     let result = this.allFilesScored;
     if (languages && languages.length > 0) {
       const lowerLangs = new Set(languages.map(l => l.toLowerCase()));
       result = result.filter(f => lowerLangs.has(f.primaryLanguage));
     }
     return result.slice(0, limit);
   }

  /**
   * Get entry points detected in the codebase
   */
  getEntryPoints(): EntryPoint[] {
    return this.codebaseStats?.entryPoints ?? [];
  }

  /**
   * Get reference summary (files by reference activity)
   */
  getReferenceSummary(maxFiles?: number): ReferenceSummaryEntry[] {
    if (!this.referenceGraph) return [];
    return this.referenceGraph.getReferenceSummary(maxFiles ?? 10);
  }

  /**
   * Get scan metrics
   */
  getMetrics(): LSPMetrics {
    return {
      scanDurationMs: this.scanDurationMs,
      filesProcessed: this.codebaseStats?.totalFiles ?? 0,
      symbolsIndexed: this.symbolIndexer.getSymbolCount(),
      languagesDetected: this.codebaseStats?.languages?.size ?? 0,
      edgesBuilt: this.referenceGraph.getTotalEdges()
    };
  }

  // ============================================================================
  // EMERGENCY CLEANUP
  // ============================================================================

  /**
   * Kill all LSP processes immediately (emergency stop)
   */
  async kill(): Promise<void> {
    await this.serverRegistry.killAll();
  }

  /**
   * Alias for kill() - kills all LSP processes
   */
  async killAll(): Promise<void> {
    await this.kill();
  }

  /**
   * Get all symbol names
   */
  getAllSymbolNames(): string[] {
    return this.symbolIndexer.getAllSymbolNames();
  }

  /**
   * Search symbol map for matching symbols
   */
  async findSymbol(name: string, kind?: SymbolKind): Promise<Array<{ file: string; line: number; character: number; name: string }>> {
    if (this.symbolIndexer.getSymbolCount() === 0) {
      await this.scan();
    }

    const results: Array<{ file: string; line: number; character: number; name: string }> = [];
    const targetKindName = kind !== undefined ? SymbolKindNames[kind] : undefined;

    // Search by partial match
    for (const [symbolName, infos] of this.symbolIndexer.getSymbolEntries()) {
      if (symbolName.toLowerCase().includes(name.toLowerCase())) {
        if (kind !== undefined) {
          const matching = infos.filter((i: SymbolInfo) => i.kind === targetKindName);
          for (const info of matching) {
            results.push({ file: info.file, line: info.line, character: info.character, name: info.name });
          }
        } else {
          for (const info of infos) {
            results.push({ file: info.file, line: info.line, character: info.character, name: info.name });
          }
        }
      }
    }

    // Fallback to exact match if no partial matches
    if (results.length === 0) {
      for (const [symbolName, infos] of this.symbolIndexer.getSymbolEntries()) {
        for (const info of infos) {
          if (name.toLowerCase() === symbolName.toLowerCase()) {
            if (kind !== undefined) {
              if (info.kind === targetKindName) {
                results.push({ file: info.file, line: info.line, character: info.character, name: info.name });
              }
            } else {
              results.push({ file: info.file, line: info.line, character: info.character, name: info.name });
            }
          }
        }
      }
    }

    if (results.length === 0) {
      logger.warn(`${LOG_PREFIX} findSymbol: no results for "${name}" in indexed symbols (${this.symbolIndexer.getSymbolCount()} symbols indexed)`);
    }

    return results.slice(0, 100);
  }

  /**
   * Find files that call/reference a symbol
   */
  async findCallers(name: string, file?: string): Promise<Array<{ file: string; line: number; symbol: string }>> {
    if (this.symbolIndexer.getSymbolCount() === 0) {
      await this.scan();
    }

    const results: Array<{ file: string; line: number; symbol: string }> = [];

    // Find the definition
    const definitions = this.symbolIndexer.getSymbolInfo(name);
    let firstDef = definitions[0];
    if (file) {
      const fileMatch = definitions.find(d => d.file === file);
      if (fileMatch) firstDef = fileMatch;
    }
    const targetFile = file || firstDef?.file;

    if (!targetFile) {
      logger.warn(`${LOG_PREFIX} findCallers: no definition found for "${name}"${file ? ` in ${file}` : ''}`);
      return results;
    }

    // Find all files that reference this file
    const referencingFiles = this.getFileReferencedBy(targetFile);

    for (const refFile of referencingFiles) {
      // Try to find the specific line where the symbol is used
      try {
        const symbols = await this.getDocumentSymbols(refFile);
        for (const symbol of symbols) {
          if (symbol.name === name || symbol.children?.some(c => c.name === name)) {
            results.push({ file: refFile, line: symbol.range?.start?.line ?? 0, symbol: symbol.name });
          }
        }
      } catch {
        // If we can't get symbols, just add the file
        results.push({ file: refFile, line: 0, symbol: name });
      }
    }

    return results;
  }

  /**
   * Get exported symbols from a file
   */
  async getExports(path: string): Promise<Array<{ name: string; kind: SymbolKind; line: number }>> {
    const symbols = await this.getDocumentSymbols(path);
    const exports: Array<{ name: string; kind: SymbolKind; line: number }> = [];

    for (const symbol of symbols) {
      // Exportable kinds: functions, classes, interfaces, variables, consts
      if ([
        SymbolKind.Function,
        SymbolKind.Class,
        SymbolKind.Interface,
        SymbolKind.Variable,
        SymbolKind.Constant,
        SymbolKind.Enum
      ].includes(symbol.kind)) {
        exports.push({
          name: symbol.name,
          kind: symbol.kind,
          line: symbol.range?.start?.line ?? 0
        });
      }
    }

    return exports;
  }

  /**
   * Get imports/exports/classes/functions summary
   */
  async getFileSummary(path: string): Promise<{ imports: string[]; exports: Array<{ name: string; kind: SymbolKind }>; classes: string[]; functions: string[] }> {
    const symbols = await this.getDocumentSymbols(path);

    const imports: string[] = [];
    const exports: Array<{ name: string; kind: SymbolKind }> = [];
    const classes: string[] = [];
    const functions: string[] = [];

    for (const symbol of symbols) {
      if (symbol.kind === SymbolKind.Class) {
        classes.push(symbol.name);
        exports.push({ name: symbol.name, kind: SymbolKind.Class });
      } else if (symbol.kind === SymbolKind.Function) {
        functions.push(symbol.name);
        exports.push({ name: symbol.name, kind: SymbolKind.Function });
      } else if ([SymbolKind.Interface, SymbolKind.Variable, SymbolKind.Constant, SymbolKind.Enum].includes(symbol.kind)) {
        exports.push({ name: symbol.name, kind: symbol.kind });
      }
    }

    return { imports, exports, classes, functions };
  }

  /**
   * Navigate import relationships
   */
  async navigateImports(fromFile?: string, toFile?: string): Promise<Array<{ file: string; imports: string[]; exports: Array<{ name: string; kind: SymbolKind }> }>> {
    if (this.symbolIndexer.getSymbolCount() === 0) {
      await this.scan();
    }

    const results: Array<{ file: string; imports: string[]; exports: Array<{ name: string; kind: SymbolKind }> }> = [];

    if (fromFile) {
      // Get imports from specific file
      const exports = await this.getExports(fromFile);
      results.push({ file: fromFile, imports: [], exports });

      // Find files that reference this file
      const refs = this.getFileReferences(fromFile);
      for (const refFile of refs) {
        if (!toFile || refFile === toFile) {
          const refExports = await this.getExports(refFile);
          results.push({ file: refFile, imports: [fromFile], exports: refExports });
        }
      }
    } else if (toFile) {
      // Get files that import into this file
      const refs = this.getFileReferencedBy(toFile);
      for (const refFile of refs) {
        const exports = await this.getExports(refFile);
        results.push({ file: refFile, imports: [toFile], exports });
      }
    } else {
      // Get all files with their exports
      const allFiles = new Set<string>();
      for (const [_, infos] of this.symbolIndexer.getSymbolEntries()) {
        for (const info of infos) {
          allFiles.add(info.file);
        }
      }

      for (const file of allFiles) {
        const exports = await this.getExports(file);
        const refs = this.getFileReferences(file);
        results.push({ file, imports: Array.from(refs), exports });
      }
    }

    return results;
  }

  // ============================================================================
  // PRIVATE METHODS
  // ============================================================================

  private async findFilesForLanguage(language: string): Promise<string[]> {
    const result = await this.host.exec(
      `find "${this.rootUri}" -type f -name "*.${language}" 2>/dev/null | head -n 100`,
      { cwd: this.rootUri }
    );
    return result.stdout.trim().split('\n').filter(f => f);
  }

  private handleFilesChanged(files: string[]): void {
    logger.info(`${LOG_PREFIX} Processing ${files.length} changed files`);

    for (const file of files) {
      // Invalidate caches for changed files
      this.referenceGraph.invalidateGraphCache();

      // Re-index the file
      const ext = file.split('.').pop() || '';
      this.symbolIndexer.processFileForSymbols(file, ext).then(result => {
        for (const symbol of result.symbols) {
          this.symbolIndexer.addSymbol(symbol);
        }
      }).catch(e => {
        logger.debug(`${LOG_PREFIX} Error re-indexing ${file}: ${e}`);
      });
    }
  }
}

// ============================================================================
// EXPORTS
// ============================================================================

export type { LSPServerConfig } from './LSPServerRegistry';
export type { SymbolInfo } from './SymbolIndexer';
export type { TopFileResult, ReferenceSummaryEntry } from './ReferenceGraph';
export type { EntryPoint, FileStats } from './FileScanner';
export type { DetectionResult } from './LanguageDetector';
export type { LuauWorkspaceInfo } from './LanguageHandler';

export default LSPUtility;
