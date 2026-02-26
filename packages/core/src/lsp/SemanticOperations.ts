import type {
  LSPLocation,
  DocumentSymbol,
  Hover,
  CallHierarchyNode,
  ReferencesGraph
} from "@firmius/shared/types";
import { SymbolKind } from "@firmius/shared/types";
import { logger } from "@firmius/shared/utils";

// Constants
const DEFAULT_REQUEST_TIMEOUT = 5000;
const FALLBACK_REQUEST_TIMEOUT = 3000;
const MAX_FALLBACK_SEARCH_LINES = 50;
const DEFAULT_CALL_HIERARCHY_DEPTH = 2;
const DEFAULT_REFERENCES_GRAPH_DEPTH = 1;

// Logger prefix
const LOGGER_PREFIX = "[LSPUtility:SemanticOps]";

// Interfaces
export interface ISemanticOperationsClient {
  request(method: string, params: any, timeout?: number): Promise<any>;
}

export interface ISemanticOperationsRegistry {
  getClientForFile(filePath: string): Promise<ISemanticOperationsClient | undefined>;
  openDocument(filePath: string, content: string): Promise<void>;
}

export interface ISemanticOperationsHost {
  readFile(path: string): Promise<string>;
}

/**
 * Provides semantic LSP operations with fallback search capabilities
 */
export class SemanticOperations {
  private registry: ISemanticOperationsRegistry;
  private host: ISemanticOperationsHost;

  constructor(registry: ISemanticOperationsRegistry, host: ISemanticOperationsHost) {
    this.registry = registry;
    this.host = host;
  }

  // ============================================================================
  // Public Semantic Methods
  // ============================================================================

  /**
   * Get definition locations for a symbol at the given position
   */
  async getDefinition(path: string, line: number, character: number): Promise<LSPLocation[]> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return [];
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const lines = content.split('\n');

    // Try the given position first
    try {
      const result = await client.request("textDocument/definition", {
        textDocument: { uri: `file://${path}` },
        position: { line, character }
      }, DEFAULT_REQUEST_TIMEOUT);
      if (result && (!Array.isArray(result) || result.length > 0)) {
        return result;
      }
    } catch { /* continue to fallback */ }

    // Search nearby lines for identifiers and try each position
    const searchStart = Math.max(0, line - 10);
    const searchEnd = Math.min(lines.length, line + MAX_FALLBACK_SEARCH_LINES);
    const wordChar = /[a-zA-Z0-9_]/;

    for (let ln = searchStart; ln < searchEnd; ln++) {
      const lineContent = lines[ln];
      if (!lineContent) continue;

      for (let i = 0; i < lineContent.length; i++) {
        if (wordChar.test(lineContent[i] || '')) {
          try {
            const result = await client.request("textDocument/definition", {
              textDocument: { uri: `file://${path}` },
              position: { line: ln, character: i }
            }, FALLBACK_REQUEST_TIMEOUT);
            if (result && (!Array.isArray(result) || result.length > 0)) {
              return result;
            }
          } catch { /* continue */ }
          while (i < lineContent.length && wordChar.test(lineContent[i] || '')) i++;
        }
      }
    }

    return [];
  }

  /**
   * Get hover information for a symbol at the given position
   */
  async getHover(path: string, line: number, character: number): Promise<Hover | null> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return null;
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const lines = content.split('\n');

    // Try the given position first
    try {
      const result = await client.request("textDocument/hover", {
        textDocument: { uri: `file://${path}` },
        position: { line, character }
      }, DEFAULT_REQUEST_TIMEOUT);
      if (result) return result;
    } catch { /* continue to fallback */ }

    // Search nearby lines for identifiers
    const searchStart = Math.max(0, line - 10);
    const searchEnd = Math.min(lines.length, line + MAX_FALLBACK_SEARCH_LINES);
    const wordChar = /[a-zA-Z0-9_]/;

    for (let ln = searchStart; ln < searchEnd; ln++) {
      const lineContent = lines[ln];
      if (!lineContent) continue;

      for (let i = 0; i < lineContent.length; i++) {
        if (wordChar.test(lineContent[i] || '')) {
          try {
            const result = await client.request("textDocument/hover", {
              textDocument: { uri: `file://${path}` },
              position: { line: ln, character: i }
            }, FALLBACK_REQUEST_TIMEOUT);
            if (result) return result;
          } catch { /* continue */ }
          while (i < lineContent.length && wordChar.test(lineContent[i] || '')) i++;
        }
      }
    }

    return null;
  }

  /**
   * Get all references to a symbol at the given position
   */
  async getReferences(path: string, line: number, character: number): Promise<LSPLocation[]> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return [];
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const lines = content.split('\n');

    // Try the given position first
    try {
      const result = await client.request("textDocument/references", {
        textDocument: { uri: `file://${path}` },
        position: { line, character },
        context: { includeDeclaration: true }
      }, DEFAULT_REQUEST_TIMEOUT);
      if (result && (!Array.isArray(result) || result.length > 0)) {
        return result;
      }
    } catch { /* continue to fallback */ }

    // Search nearby lines for identifiers
    const searchStart = Math.max(0, line - 10);
    const searchEnd = Math.min(lines.length, line + MAX_FALLBACK_SEARCH_LINES);
    const wordChar = /[a-zA-Z0-9_]/;

    for (let ln = searchStart; ln < searchEnd; ln++) {
      const lineContent = lines[ln];
      if (!lineContent) continue;

      for (let i = 0; i < lineContent.length; i++) {
        if (wordChar.test(lineContent[i] || '')) {
          try {
            const result = await client.request("textDocument/references", {
              textDocument: { uri: `file://${path}` },
              position: { line: ln, character: i },
              context: { includeDeclaration: true }
            }, FALLBACK_REQUEST_TIMEOUT);
            if (result && (!Array.isArray(result) || result.length > 0)) {
              return result;
            }
          } catch { /* continue */ }
          while (i < lineContent.length && wordChar.test(lineContent[i] || '')) i++;
        }
      }
    }

    return [];
  }

  /**
   * Get document symbols for a file
   */
  async getDocumentSymbols(path: string, kinds?: SymbolKind[]): Promise<DocumentSymbol[]> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return [];
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const symbols = await client.request("textDocument/documentSymbol", {
      textDocument: { uri: `file://${path}` }
    });

    if (!Array.isArray(symbols)) return [];

    if (!kinds || kinds.length === 0) return symbols;

    const allowedKinds = new Set(kinds);
    return this.filterDocumentSymbols(symbols, allowedKinds);
  }

  /**
   * Get call hierarchy for a symbol
   */
  async getCallHierarchy(
    path: string,
    line: number,
    character: number,
    direction: 'incoming' | 'outgoing',
    maxDepth: number = DEFAULT_CALL_HIERARCHY_DEPTH
  ): Promise<CallHierarchyNode[]> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return [];
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const hasCallHierarchy = await this.checkCallHierarchyCapability(client);

    if (!hasCallHierarchy) {
      logger.warn(`${LOGGER_PREFIX} callHierarchy not supported, using references fallback`);
      return this.buildCallHierarchyFallback(path, line, character, client, direction, maxDepth);
    }

    let items = await client.request("textDocument/prepareCallHierarchy", {
      textDocument: { uri: `file://${path}` },
      position: { line, character }
    });

    if (!items || items.length === 0) {
      const bestPosition = await this.findBestSymbolPosition(path, line);
      if (bestPosition) {
        logger.warn(`${LOGGER_PREFIX} Using nearest symbol at ${bestPosition.line}:${bestPosition.character}`);
        const fallbackItems = await client.request("textDocument/prepareCallHierarchy", {
          textDocument: { uri: `file://${path}` },
          position: bestPosition
        });
        if (fallbackItems && fallbackItems.length > 0) {
          items = fallbackItems;
        }
      }
    }

    if (!items || items.length === 0) return [];

    const results: CallHierarchyNode[] = [];
    for (const item of items) {
      const node = await this.buildCallHierarchyTree(item, client, direction, 0, maxDepth, new Set());
      results.push(node);
    }

    return results;
  }

  /**
   * Get references graph for a symbol
   */
  async getReferencesGraph(
    path: string,
    line: number,
    character: number,
    maxDepth: number = DEFAULT_REFERENCES_GRAPH_DEPTH
  ): Promise<ReferencesGraph> {
    const client = await this.registry.getClientForFile(path);
    if (!client) {
      logger.warn(`${LOGGER_PREFIX} Failed to get LSP client for ${path}`);
      return {
        nodes: [],
        edges: [],
        symbol: 'unknown',
        location: { path, line, character }
      };
    }

    const content = await this.host.readFile(path);
    await this.registry.openDocument(path, content);

    const references = await client.request("textDocument/references", {
      textDocument: { uri: `file://${path}` },
      position: { line, character },
      context: { includeDeclaration: true }
    });

    let symbolName = 'unknown';
    try {
      const definitions = await client.request("textDocument/definition", {
        textDocument: { uri: `file://${path}` },
        position: { line, character }
      });
      if (definitions && definitions.length > 0) {
        const defClient = await this.registry.getClientForFile(definitions[0].uri.replace(/^file:\/\//, ''));
        if (defClient) {
          const symbols = await defClient.request("textDocument/documentSymbol", {
            textDocument: { uri: definitions[0].uri }
          });
          if (Array.isArray(symbols)) {
            for (const s of symbols) {
              const sLine = s.range?.start?.line ?? -1;
              if (sLine === definitions[0].range.start.line) {
                symbolName = s.name;
                break;
              }
            }
          }
        }
      }
    } catch { /* use default 'unknown' */ }

    const { nodes, edges } = await this.buildReferencesGraph(references, 0, maxDepth, new Set());

    const MAX_GRAPH_NODES = 500;
    const MAX_GRAPH_EDGES = 1000;

    const cappedNodes = nodes.slice(0, MAX_GRAPH_NODES);
    const nodeIds = new Set(cappedNodes.map(n => n.id));
    const cappedEdges = edges.filter(e => nodeIds.has(e.from) && nodeIds.has(e.to)).slice(0, MAX_GRAPH_EDGES);

    return {
      nodes: cappedNodes,
      edges: cappedEdges,
      symbol: symbolName,
      location: { path, line, character }
    };
  }

  // ============================================================================
  // Helper Methods
  // ============================================================================

  /**
   * Find the best symbol position near a target line
   */
  async findBestSymbolPosition(path: string, targetLine: number): Promise<{ line: number; character: number } | null> {
    const client = await this.registry.getClientForFile(path);
    if (!client) return null;

    try {
      const symbols = await client.request("textDocument/documentSymbol", {
        textDocument: { uri: `file://${path}` }
      });

      if (!Array.isArray(symbols) || symbols.length === 0) return null;

      let bestMatch: { line: number; character: number } | null = null;
      let minDistance = Infinity;

      const scanSymbols = (symbolList: DocumentSymbol[]): void => {
        for (const sym of symbolList) {
          const range = sym.range;
          if (!range) continue;

          const startLine = range.start.line;
          const endLine = range.end.line;

          if (targetLine >= startLine && targetLine <= endLine) {
            bestMatch = { line: startLine, character: range.start.character };
            minDistance = 0;
            return;
          }

          const distance = Math.min(Math.abs(targetLine - startLine), Math.abs(targetLine - endLine));
          if (distance < minDistance) {
            minDistance = distance;
            bestMatch = { line: startLine, character: range.start.character };
          }

          if (sym.children && sym.children.length > 0) {
            scanSymbols(sym.children);
          }
        }
      };

      scanSymbols(symbols);

      if (bestMatch && minDistance <= 10) return bestMatch;
      if (bestMatch && symbols.length > 0) {
        const firstRange = symbols[0].range;
        if (firstRange) return { line: firstRange.start.line, character: firstRange.start.character };
      }

      return null;
    } catch {
      return null;
    }
  }

  /**
   * Filter document symbols by allowed kinds
   */
  private filterDocumentSymbols(symbols: DocumentSymbol[], allowedKinds: Set<SymbolKind>): DocumentSymbol[] {
    const filtered: DocumentSymbol[] = [];
    for (const sym of symbols) {
      if (allowedKinds.has(sym.kind)) {
        filtered.push(sym);
      }
      if (sym.children && sym.children.length > 0) {
        const filteredChildren = this.filterDocumentSymbols(sym.children, allowedKinds);
        if (filteredChildren.length > 0) {
          filtered.push({ ...sym, children: filteredChildren });
        }
      }
    }
    return filtered;
  }

  /**
   * Check if the client supports call hierarchy capability
   */
  private async checkCallHierarchyCapability(client: ISemanticOperationsClient): Promise<boolean> {
    try {
      const capabilities = await client.request("initialize", {
        processId: null,
        rootUri: null,
        capabilities: {}
      });
      return !!(capabilities?.capabilities?.callHierarchyProvider);
    } catch {
      return false;
    }
  }

  /**
   * Build call hierarchy tree recursively
   */
  private async buildCallHierarchyTree(
    item: any,
    client: ISemanticOperationsClient,
    direction: 'incoming' | 'outgoing',
    currentDepth: number,
    maxDepth: number,
    visited: Set<string>
  ): Promise<CallHierarchyNode> {
    const name = item.name || 'Unknown';
    const uri = item.uri || item.location?.uri;
    const range = item.range || item.selectionRange;

    const filePath = uri ? uri.replace(/^file:\/\//, '') : '';
    const line = range?.start?.line ?? 0;
    const character = range?.start?.character ?? 0;

    const nodeKey = `${name}:${filePath}:${line}:${character}`;
    const children: CallHierarchyNode[] = [];

    if (currentDepth < maxDepth && !visited.has(nodeKey)) {
      visited.add(nodeKey);

      try {
        const method = direction === 'incoming' ? 'callHierarchy/incomingCalls' : 'callHierarchy/outgoingCalls';
        const calls = await client.request(method, { item });

        if (Array.isArray(calls)) {
          for (const call of calls) {
            const child = await this.buildCallHierarchyTree(
              direction === 'incoming' ? call.from : call.to,
              client,
              direction,
              currentDepth + 1,
              maxDepth,
              visited
            );
            children.push(child);
          }
        }
      } catch { /* ignore errors in recursive calls */ }
    }

    return { symbol: name, file: filePath, line, character, children };
  }

  /**
   * Check if a file is a generated file
   */
  private isGeneratedFile(filePath: string): boolean {
    return filePath.includes('/generated/') ||
      filePath.includes('/.sandbox/') ||
      filePath.endsWith('.min.js') ||
      filePath.includes('_pb.') ||
      filePath.includes('.d.ts');
  }

  /**
   * Build call hierarchy fallback using references when callHierarchy is not supported
   */
  private async buildCallHierarchyFallback(
    path: string,
    line: number,
    character: number,
    client: ISemanticOperationsClient,
    direction: 'incoming' | 'outgoing',
    maxDepth: number
  ): Promise<CallHierarchyNode[]> {
    if (this.isGeneratedFile(path)) {
      logger.warn(`${LOGGER_PREFIX} Skipping call hierarchy for generated file: ${path}`);
      return [];
    }

    const references = await client.request("textDocument/references", {
      textDocument: { uri: `file://${path}` },
      position: { line, character },
      context: { includeDeclaration: true }
    });

    let symbolName = 'Unknown';
    let symbolFile = path;
    let symbolLine = line;
    let symbolChar = character;

    if (references && references.length > 0) {
      const def = references.find((r: any) => {
        const range = r.range || r.location?.range;
        return range?.start?.line === line && range?.start?.character === character;
      });
      if (def) {
        try {
          const definitions = await client.request("textDocument/definition", {
            textDocument: { uri: `file://${path}` },
            position: { line, character }
          });
          if (definitions && definitions.length > 0) {
            const uri = definitions[0].uri || definitions[0].location?.uri;
            const range = definitions[0].range || definitions[0].location?.range;
            if (uri && range) {
              symbolFile = uri.replace(/^file:\/\//, '');
              symbolLine = range.start.line;
              symbolChar = range.start.character;
              const client2 = await this.registry.getClientForFile(symbolFile);
              if (client2) {
                const symbols = await client2.request("textDocument/documentSymbol", {
                  textDocument: { uri }
                });
                if (Array.isArray(symbols)) {
                  for (const s of symbols) {
                    const sLine = s.range?.start?.line ?? -1;
                    if (sLine === symbolLine) {
                      symbolName = s.name;
                      break;
                    }
                  }
                }
              }
            }
          }
        } catch { /* use defaults */ }
      }
    }

    const nodeKey = `${symbolName}:${symbolFile}:${symbolLine}:${symbolChar}`;
    const children: CallHierarchyNode[] = [];

    if (maxDepth > 0 && references) {
      const visited = new Set<string>();
      visited.add(nodeKey);

      if (Array.isArray(references)) {
        for (const ref of references) {
          const refUri = ref.uri || ref.location?.uri;
          const refRange = ref.range || ref.location?.range;
          if (!refUri || !refRange) continue;

          const refFile = refUri.replace(/^file:\/\//, '');
          const refLine = refRange.start.line;
          const refChar = refRange.start.character;

          if (this.isGeneratedFile(refFile)) continue;

          const childKey = `${refFile}:${refLine}:${refChar}`;
          if (visited.has(childKey)) continue;

          let childName = 'Unknown';
          try {
            const client2 = await this.registry.getClientForFile(refFile);
            if (client2) {
              const symbols = await client2.request("textDocument/documentSymbol", {
                textDocument: { uri: refUri }
              });
              if (Array.isArray(symbols)) {
                for (const s of symbols) {
                  const sLine = s.range?.start?.line ?? -1;
                  const eLine = s.range?.end?.line ?? sLine;
                  if (refLine >= sLine && refLine <= eLine) {
                    childName = s.name;
                    break;
                  }
                }
              }
            }
          } catch { /* use 'Unknown' */ }

          const isIncoming = direction === 'incoming';
          const shouldInclude = isIncoming ? (refFile !== path || refLine !== line) : true;

          if (shouldInclude) {
            children.push({ symbol: childName, file: refFile, line: refLine, character: refChar, children: [] });
          }
        }
      }
    }

    const root: CallHierarchyNode = { symbol: symbolName, file: symbolFile, line: symbolLine, character: symbolChar, children };
    return children.length > 0 || symbolName !== 'Unknown' ? [root] : [];
  }

  /**
   * Build references graph recursively
   */
  private async buildReferencesGraph(
    references: LSPLocation[],
    currentDepth: number,
    maxDepth: number,
    visited: Set<string>
  ): Promise<{ nodes: Array<{ id: string; file: string; symbol: string; line: number }>; edges: Array<{ from: string; to: string; type: string }> }> {
    const nodes: Array<{ id: string; file: string; symbol: string; line: number }> = [];
    const edges: Array<{ from: string; to: string; type: string }> = [];

    for (const ref of references) {
      const filePath = ref.uri.replace(/^file:\/\//, '');
      const nodeId = `${filePath}:${ref.range.start.line}`;

      if (visited.has(nodeId)) continue;
      visited.add(nodeId);

      nodes.push({ id: nodeId, file: filePath, symbol: 'unknown', line: ref.range.start.line });

      if (currentDepth < maxDepth) {
        try {
          const client = await this.registry.getClientForFile(filePath);
          if (client) {
            const symbols = await client.request("textDocument/documentSymbol", {
              textDocument: { uri: ref.uri }
            });

            if (Array.isArray(symbols)) {
              for (const symbol of symbols) {
                const symbolId = `${filePath}:${symbol.range?.start?.line ?? 0}:${symbol.name}`;
                if (visited.has(symbolId)) continue;

                nodes.push({ id: symbolId, file: filePath, symbol: symbol.name, line: symbol.range?.start?.line ?? 0 });
                edges.push({ from: nodeId, to: symbolId, type: 'contains' });
              }
            }
          }
        } catch { /* continue */ }
      }
    }

    return { nodes, edges };
  }
}

export default SemanticOperations;
