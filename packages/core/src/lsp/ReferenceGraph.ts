import { logger } from "@firmius/shared/utils";

const GRAPH_CACHE_TTL_MS = 10 * 60 * 1000;
const MAX_SYMBOLS_TO_PROCESS = 30;
const MAX_REFS_PER_SYMBOL = 20;
const MAX_TOTAL_EDGES = 200;

interface SymbolInfo {
  name: string;
  kind: string;
  file: string;
  line: number;
  character: number;
}

interface ReferenceGraphCache {
  graph: Map<string, Set<string>>;
  reverseGraph: Map<string, Set<string>>;
  timestamp: number;
  totalSymbolsProcessed: number;
  totalEdges: number;
}

export interface TopFileResult {
  path: string;
  symbolCount: number;
  referenceCount: number;
  score: number;
  primaryLanguage: string;
}

export interface ReferenceSummaryEntry {
  file: string;
  outgoingRefs: number;
  incomingRefs: number;
}

export interface ReferenceGraphData {
  forward: Map<string, Set<string>>;
  reverse: Map<string, Set<string>>;
  timestamp: number;
}

interface SymbolMap {
  entries(): IterableIterator<[string, SymbolInfo[]]>;
  get(key: string): SymbolInfo[] | undefined;
  values(): IterableIterator<SymbolInfo[]>;
}

interface LSPClientProvider {
  getClientForFile(filePath: string): Promise<any>;
  openDocument(path: string, content: string): Promise<void>;
}

interface HostProvider {
  readFile(path: string): Promise<string>;
}

const SKIP_DIRS = new Set([
  'node_modules', '.git', '.cache', 'dist', 'build', 'out', 'target',
  '__pycache__', '.tox', '.venv', 'venv', '.idea', '.vscode', 'bin', 'obj',
  'third_party', 'thirdparty', '3rdparty', '3rd_party',
  'vendor', 'vendors', 'external', 'extern', 'externals',
  'deps', 'dependencies', '_deps', '.submodules', 'submodules',
  'packages', 'pkg', '.nox', '.mypy_cache', 'coverage', '.pytest_cache',
  'gen', 'generated', 'protobuf', 'proto', '.pb.go'
]);

const LANGUAGE_MAP: Record<string, string> = {
  'ts': 'typescript',
  'js': 'javascript',
  'py': 'python',
  'rs': 'rust',
  'cpp': 'cpp',
  'c': 'c',
  'go': 'go',
  'luau': 'luau',
  'lua': 'lua'
};

export class ReferenceGraph {
  private referenceGraph: Map<string, Set<string>> = new Map();
  private reverseReferenceGraph: Map<string, Set<string>> = new Map();
  private symbolToFiles: Map<string, Set<string>> = new Map();
  private referenceGraphCache: ReferenceGraphCache | null = null;
  private symbolMap: SymbolMap;
  private lspClient: LSPClientProvider;
  private host: HostProvider;

  constructor(
    symbolMap: SymbolMap,
    lspClient: LSPClientProvider,
    host: HostProvider
  ) {
    this.symbolMap = symbolMap;
    this.lspClient = lspClient;
    this.host = host;
  }

  clear(): void {
    this.referenceGraph.clear();
    this.reverseReferenceGraph.clear();
    this.symbolToFiles.clear();
    this.referenceGraphCache = null;
    logger.info("[ReferenceGraph] Cache cleared");
  }

  setSymbolMap(symbolMap: SymbolMap): void {
    this.symbolMap = symbolMap;
  }

  addSymbol(symbol: SymbolInfo): void {
    if (!this.symbolToFiles.has(symbol.name)) {
      this.symbolToFiles.set(symbol.name, new Set());
    }
    this.symbolToFiles.get(symbol.name)!.add(symbol.file);
  }

  getSymbolFiles(symbolName: string): Set<string> {
    return this.symbolToFiles.get(symbolName) || new Set();
  }

  async buildReferenceGraph(): Promise<void> {
    this.referenceGraph.clear();
    this.reverseReferenceGraph.clear();

    const allSymbols = Array.from(this.symbolMap.entries())
      .sort((a, b) => b[1].length - a[1].length)
      .slice(0, MAX_SYMBOLS_TO_PROCESS);

    logger.info(`[ReferenceGraph] Building reference graph from top ${allSymbols.length} symbols (max ${MAX_REFS_PER_SYMBOL} refs each, max ${MAX_TOTAL_EDGES} total edges)...`);

    let totalEdges = 0;
    let symbolsProcessed = 0;

    for (const [symbolName] of allSymbols) {
      if (totalEdges >= MAX_TOTAL_EDGES) {
        logger.warn(`[ReferenceGraph] Reference graph capped at ${MAX_TOTAL_EDGES} edges to prevent agent overwhelm`);
        break;
      }

      const definitions = this.symbolMap.get(symbolName);
      if (!definitions || definitions.length === 0) continue;

      const definition = definitions[0];
      if (!definition) continue;

      const client = await this.lspClient.getClientForFile(definition.file);
      if (!client) continue;

      let references: any[] | null = null;
      try {
        const content = await this.host.readFile(definition.file);
        await this.lspClient.openDocument(definition.file, content);

        references = await client.request("textDocument/references", {
          textDocument: { uri: `file://${definition.file}` },
          position: { line: definition.line, character: definition.character },
          context: { includeDeclaration: false }
        }, 3000);
      } catch {
        continue;
      }

      if (references && Array.isArray(references)) {
        const limitedRefs = references.slice(0, MAX_REFS_PER_SYMBOL);

        for (const ref of limitedRefs) {
          if (totalEdges >= MAX_TOTAL_EDGES) break;

          const refFile = ref.uri.replace(/^file:\/\//, "");

          if (!this.referenceGraph.has(definition.file)) {
            this.referenceGraph.set(definition.file, new Set());
          }
          this.referenceGraph.get(definition.file)!.add(refFile);

          if (!this.reverseReferenceGraph.has(refFile)) {
            this.reverseReferenceGraph.set(refFile, new Set());
          }
          this.reverseReferenceGraph.get(refFile)!.add(definition.file);

          totalEdges++;
        }

        symbolsProcessed++;
      }
    }

    logger.info(`[ReferenceGraph] Built reference graph: ${totalEdges} edges from ${symbolsProcessed}/${allSymbols.length} symbols processed`);

    this.referenceGraphCache = {
      graph: new Map(this.referenceGraph),
      reverseGraph: new Map(this.reverseReferenceGraph),
      timestamp: Date.now(),
      totalSymbolsProcessed: symbolsProcessed,
      totalEdges
    };
  }

  getReferenceSummary(maxFiles: number = 10): ReferenceSummaryEntry[] {
    const fileStats = new Map<string, { outgoing: number; incoming: number }>();

    for (const [file, refs] of this.referenceGraph.entries()) {
      const stats = fileStats.get(file) || { outgoing: 0, incoming: 0 };
      stats.outgoing = refs.size;
      fileStats.set(file, stats);
    }

    for (const [file, refs] of this.reverseReferenceGraph.entries()) {
      const stats = fileStats.get(file) || { outgoing: 0, incoming: 0 };
      stats.incoming = refs.size;
      fileStats.set(file, stats);
    }

    return Array.from(fileStats.entries())
      .map(([file, stats]) => ({
        file,
        outgoingRefs: stats.outgoing,
        incomingRefs: stats.incoming
      }))
      .sort((a, b) => (b.outgoingRefs + b.incomingRefs) - (a.outgoingRefs + a.incomingRefs))
      .slice(0, maxFiles);
  }

  private isThirdPartyFile(filePath: string): boolean {
    const parts = filePath.split('/');
    return parts.some(part => SKIP_DIRS.has(part.toLowerCase()));
  }

  private isGraphStale(): boolean {
    if (!this.referenceGraphCache) return true;
    const age = Date.now() - this.referenceGraphCache.timestamp;
    return age > GRAPH_CACHE_TTL_MS;
  }

  invalidateGraphCache(): void {
    if (this.referenceGraphCache) {
      logger.info(`[ReferenceGraph] Graph cache invalidated`);
    }
    this.referenceGraphCache = null;
  }

  getReferenceGraph(): ReferenceGraphData | null {
    if (!this.referenceGraphCache) return null;
    return {
      forward: this.referenceGraphCache.graph,
      reverse: this.referenceGraphCache.reverseGraph,
      timestamp: this.referenceGraphCache.timestamp
    };
  }

  private getCachedReferenceCounts(): Map<string, number> {
    const counts = new Map<string, number>();
    if (this.referenceGraphCache) {
      for (const [file, refs] of this.referenceGraphCache.reverseGraph.entries()) {
        counts.set(file, refs.size);
      }
    }
    return counts;
  }

  getTopFiles(limit: number = 20): TopFileResult[] {
    const allFiles = new Set<string>();
    for (const symbols of this.symbolMap.values()) {
      for (const sym of symbols) {
        allFiles.add(sym.file);
      }
    }

    if (allFiles.size === 0) {
      return [];
    }

    const results: TopFileResult[] = [];
    let thirdPartyCount = 0;
    let projectFileCount = 0;

    const refCounts = this.getCachedReferenceCounts();

    for (const file of allFiles) {
      const symbolCount = Array.from(this.symbolMap.values())
        .flat()
        .filter(s => s.file === file)
        .length;

      const referenceCount = refCounts.get(file) || 0;

      let score = (symbolCount * 2) + (referenceCount * 3);

      const isThirdParty = this.isThirdPartyFile(file);
      if (isThirdParty) {
        thirdPartyCount++;
        score = Math.floor(score * 0.1);
      } else {
        projectFileCount++;
        score = Math.floor(score * 1.5);
      }

      const language = this.getLanguageFromPath(file);

      results.push({
        path: file,
        symbolCount,
        referenceCount,
        score,
        primaryLanguage: language
      });
    }

    logger.info(`[ReferenceGraph] Scored ${projectFileCount} project files, ${thirdPartyCount} third-party files (third-party penalized)`);

    return results
      .sort((a, b) => b.score - a.score)
      .slice(0, limit);
  }

  getFileReferences(filePath: string): Set<string> {
    return this.referenceGraph.get(filePath) || new Set();
  }

  getFileReferencedBy(filePath: string): Set<string> {
    return this.reverseReferenceGraph.get(filePath) || new Set();
  }

  isStale(): boolean {
    return this.isGraphStale();
  }

  getCacheTimestamp(): number | null {
    return this.referenceGraphCache?.timestamp || null;
  }

  getTotalEdges(): number {
    return this.referenceGraphCache?.totalEdges || 0;
  }

  getTotalSymbolsProcessed(): number {
    return this.referenceGraphCache?.totalSymbolsProcessed || 0;
  }

  private getLanguageFromPath(filePath: string): string {
    const ext = filePath.split('.').pop()?.toLowerCase();
    return ext ? LANGUAGE_MAP[ext] || ext : 'unknown';
  }
}
