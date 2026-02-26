import { LRUCache, logger } from "@firmius/shared/utils";
import { SymbolKind, getSymbolKindName } from "@firmius/shared/types";

const DEFAULT_SYMBOL_LIMIT = 15000;
const LSP_REQUEST_TIMEOUT = 2000;
const DEFAULT_GO_TIMEOUT = 30000;

export interface SymbolInfo {
  name: string;
  kind: string;
  file: string;
  line: number;
  character: number;
}

export const SymbolKindNames: Record<number, string> = {
  [SymbolKind.File]: "File",
  [SymbolKind.Module]: "Module",
  [SymbolKind.Namespace]: "Namespace",
  [SymbolKind.Package]: "Package",
  [SymbolKind.Class]: "Class",
  [SymbolKind.Method]: "Method",
  [SymbolKind.Property]: "Property",
  [SymbolKind.Field]: "Field",
  [SymbolKind.Constructor]: "Constructor",
  [SymbolKind.Enum]: "Enum",
  [SymbolKind.Interface]: "Interface",
  [SymbolKind.Function]: "Function",
  [SymbolKind.Variable]: "Variable",
  [SymbolKind.Constant]: "Constant",
  [SymbolKind.String]: "String",
  [SymbolKind.Number]: "Number",
  [SymbolKind.Boolean]: "Boolean",
  [SymbolKind.Array]: "Array",
  [SymbolKind.Object]: "Object",
  [SymbolKind.Key]: "Key",
  [SymbolKind.Null]: "Null",
  [SymbolKind.EnumMember]: "EnumMember",
  [SymbolKind.Struct]: "Struct",
  [SymbolKind.Event]: "Event",
  [SymbolKind.Operator]: "Operator",
  [SymbolKind.TypeParameter]: "TypeParameter"
};

export interface ISymbolIndexerClient {
  request(method: string, params: any, timeout?: number): Promise<any>;
}

export interface ISymbolIndexerRegistry {
  getClientForFile(filePath: string): Promise<ISymbolIndexerClient | undefined>;
  openDocument(filePath: string, content: string): Promise<void>;
}

export interface ISymbolIndexerHost {
  readFile(path: string): Promise<string>;
}

export interface SymbolIndexerOptions {
  symbolLimit?: number;
}

export class SymbolIndexer {
  private symbolMap: LRUCache<SymbolInfo[]>;
  private symbolToFiles: Map<string, Set<string>>;
  private registry: ISymbolIndexerRegistry;
  private host: ISymbolIndexerHost;

  constructor(registry: ISymbolIndexerRegistry, host: ISymbolIndexerHost, options?: SymbolIndexerOptions) {
    this.registry = registry;
    this.host = host;
    this.symbolToFiles = new Map();
    this.symbolMap = new LRUCache<SymbolInfo[]>(options?.symbolLimit ?? DEFAULT_SYMBOL_LIMIT);
    this.symbolMap.setOnEvict((symbolName: string, symbolInfo: SymbolInfo[]) => {
      this.onSymbolEvicted(symbolName, symbolInfo);
    });
    logger.info("[SymbolIndexer] Initialized with LRU cache");
  }

  private onSymbolEvicted(symbolName: string, symbolInfo: SymbolInfo[]): void {
    for (const info of symbolInfo) {
      const fileSet = this.symbolToFiles.get(symbolName);
      if (fileSet) {
        fileSet.delete(info.file);
        if (fileSet.size === 0) {
          this.symbolToFiles.delete(symbolName);
        }
      }
    }
  }

  addSymbol(symbol: SymbolInfo): void {
    const existing = this.symbolMap.get(symbol.name);
    if (existing) {
      existing.push(symbol);
    } else {
      this.symbolMap.set(symbol.name, [symbol]);
    }

    if (!this.symbolToFiles.has(symbol.name)) {
      this.symbolToFiles.set(symbol.name, new Set());
    }
    this.symbolToFiles.get(symbol.name)!.add(symbol.file);
  }

  async processFileForSymbols(
    filePath: string,
    ext: string
  ): Promise<{ symbols: SymbolInfo[]; filePath: string }> {
    const client = await this.registry.getClientForFile(filePath);
    if (!client) return { symbols: [], filePath };

    const timeout = ext === 'go' ? DEFAULT_GO_TIMEOUT : LSP_REQUEST_TIMEOUT;

    try {
      const content = await this.host.readFile(filePath);
      await this.registry.openDocument(filePath, content);

      const symbols = await client.request("textDocument/documentSymbol", {
        textDocument: { uri: `file://${filePath}` }
      }, timeout);

      if (!Array.isArray(symbols) || symbols.length === 0) {
        if (ext === 'go') {
          logger.debug(`[SymbolIndexer] [${ext}] No symbols returned for ${filePath} (${symbols ? 'empty array' : 'null/undefined'})`);
        }
        return { symbols: [], filePath };
      }

      const symbolInfos: SymbolInfo[] = [];

      const processDocumentSymbol = (symbol: any, file: string) => {
        const symbolName = symbol.name;
        if (!symbolName) return;

        const range = symbol.range || symbol.selectionRange || symbol.location?.range;
        if (!range) {
          logger.debug(`[SymbolIndexer] [${ext}] Symbol "${symbolName}" has no range`);
          return;
        }

        symbolInfos.push({
          name: symbolName,
          kind: getSymbolKindName(symbol.kind),
          file: file,
          line: range.start.line,
          character: range.start.character
        });

        if (symbol.children && Array.isArray(symbol.children)) {
          for (const child of symbol.children) {
            processDocumentSymbol(child, file);
          }
        }
      };

      const processSymbolInfo = (symbol: any, file: string) => {
        const symbolName = symbol.name;
        if (!symbolName) return;

        const location = symbol.location;
        if (!location) {
          logger.debug(`[SymbolIndexer] [${ext}] SymbolInfo "${symbolName}" has no location`);
          return;
        }

        const range = location.range;
        if (!range) {
          logger.debug(`[SymbolIndexer] [${ext}] SymbolInfo "${symbolName}" has no range in location`);
          return;
        }

        symbolInfos.push({
          name: symbolName,
          kind: getSymbolKindName(symbol.kind),
          file: file,
          line: range.start.line,
          character: range.start.character
        });
      };

      for (const symbol of symbols) {
        if (symbol.children !== undefined || symbol.selectionRange !== undefined) {
          processDocumentSymbol(symbol, filePath);
        } else if (symbol.location !== undefined) {
          processSymbolInfo(symbol, filePath);
        } else {
          logger.debug(`[SymbolIndexer] [${ext}] Unknown symbol type for "${symbol.name}": ${JSON.stringify(Object.keys(symbol))}`);
        }
      }

      if (symbolInfos.length === 0) {
        logger.debug(`[SymbolIndexer] [${ext}] Processed ${symbols.length} raw symbols but got 0 SymbolInfo for ${filePath}`);
      }

      return { symbols: symbolInfos, filePath };
    } catch (e) {
      const errMsg = e instanceof Error ? e.message : String(e);
      if (errMsg.includes("No Project")) {
        logger.info(`[SymbolIndexer] Skipping .${ext} - no project configuration`);
      } else if (errMsg.includes("timeout") || errMsg.includes("timed out")) {
        if (ext === 'go') {
          logger.warn(`[SymbolIndexer] [${ext}] Timeout processing ${filePath} after ${timeout}ms - LSP may be slow or file too large`);
        }
      } else {
        logger.warn(`[SymbolIndexer] [${ext}] Error processing ${filePath}: ${errMsg}`);
      }
      return { symbols: [], filePath };
    }
  }

  getSymbolInfo(symbolName: string): SymbolInfo[] {
    return this.symbolMap.get(symbolName) || [];
  }

  getTopSymbols(limit: number = 20, kinds?: SymbolKind[]): Array<{ name: string; kind: string; file: string; line: number; character: number; callerCount: number }> {
    const symbolCallerCounts = new Map<string, { info: SymbolInfo; count: number }>();

    for (const [name, infos] of this.symbolMap.entries()) {
      for (const info of infos) {
        if (kinds && kinds.length > 0) {
          const kindNum = Object.entries(SymbolKindNames).find(([, v]) => v === info.kind)?.[0];
          if (!kindNum || !kinds.includes(parseInt(kindNum))) continue;
        }

        const key = `${name}::${info.file}::${info.line}`;
        const existing = symbolCallerCounts.get(key);
        if (!existing || existing.count < 0) {
          symbolCallerCounts.set(key, { info, count: 0 });
        }
      }
    }

    const sorted = Array.from(symbolCallerCounts.values())
      .sort((a, b) => b.count - a.count)
      .slice(0, limit);

    return sorted.map(({ info, count }) => ({
      name: info.name,
      kind: info.kind,
      file: info.file,
      line: info.line,
      character: info.character ?? 0,
      callerCount: count
    }));
  }

  clear(): void {
    this.symbolMap.clear();
    this.symbolToFiles.clear();
    logger.info("[SymbolIndexer] Cache cleared");
  }

  getSymbolCount(): number {
    return this.symbolMap.size;
  }

  getFilesForSymbol(symbolName: string): string[] {
    const files = this.symbolToFiles.get(symbolName);
    return files ? Array.from(files) : [];
  }

  hasSymbol(symbolName: string): boolean {
    return this.symbolMap.has(symbolName);
  }

  getAllSymbolNames(): string[] {
    return Array.from(this.symbolMap.keys());
  }

  getSymbolEntries(): Array<[string, SymbolInfo[]]> {
    return Array.from(this.symbolMap.entries());
  }
}
