export interface LSPRequest {
  jsonrpc: "2.0";
  id: number | string;
  method: string;
  params?: unknown;
}

export interface LSPNotification {
  jsonrpc: "2.0";
  method: string;
  params?: unknown;
}

export interface LSPResponse {
  jsonrpc: "2.0";
  id: number | string | null;
  result?: unknown;
  error?: LSPError;
}

export interface LSPError {
  code: number;
  message: string;
  data?: unknown;
}

export interface LSPPosition {
  line: number;
  character: number;
}

export interface LSPRange {
  start: LSPPosition;
  end: LSPPosition;
}

export interface LSPLocation {
  uri: string;
  range: LSPRange;
}

export interface LSPDiagnostic {
  range: LSPRange;
  severity?: number;
  code?: string | number;
  source?: string;
  message: string;
}

export enum SymbolKind {
  File = 1,
  Module = 2,
  Namespace = 3,
  Package = 4,
  Class = 5,
  Method = 6,
  Property = 7,
  Field = 8,
  Constructor = 9,
  Enum = 10,
  Interface = 11,
  Function = 12,
  Variable = 13,
  Constant = 14,
  String = 15,
  Number = 16,
  Boolean = 17,
  Array = 18,
  Object = 19,
  Key = 20,
  Null = 21,
  EnumMember = 22,
  Struct = 23,
  Event = 24,
  Operator = 25,
  TypeParameter = 26
}

export interface DocumentSymbol {
  name: string;
  detail?: string;
  kind: SymbolKind;
  deprecated?: boolean;
  range: LSPRange;
  selectionRange: LSPRange;
  children?: DocumentSymbol[];
}

export interface SymbolInformation {
  name: string;
  kind: SymbolKind;
  deprecated?: boolean;
  location: LSPLocation;
  containerName?: string;
}

export type SymbolResponse = DocumentSymbol[] | SymbolInformation[] | null | undefined;

export enum MarkupKind {
  PlainText = "plaintext",
  Markdown = "markdown"
}

export interface LSPMarkupContent {
  kind: MarkupKind;
  value: string;
}

export type LSPMarkedString = string | { language: string; value: string };

export interface Hover {
  contents: LSPMarkupContent | LSPMarkedString | LSPMarkedString[];
  range?: LSPRange;
}

export interface CallHierarchyItem {
  name: string;
  kind: SymbolKind;
  uri: string;
  range: LSPRange;
  selectionRange: LSPRange;
}

export interface CallHierarchyIncomingCall {
  from: CallHierarchyItem;
  fromRanges: LSPRange[];
}

export interface CallHierarchyOutgoingCall {
  to: CallHierarchyItem;
  fromRanges: LSPRange[];
}

export interface TextDocumentPositionParams {
  textDocument: { uri: string };
  position: LSPPosition;
}

export interface ReferenceContext {
  includeDeclaration: boolean;
}

export interface CallHierarchyNode {
  symbol: string;
  file: string;
  line: number;
  character: number;
  children: CallHierarchyNode[];
}

export interface ReferencesGraph {
  nodes: Array<{ id: string; file: string; symbol: string; line: number }>;
  edges: Array<{ from: string; to: string; type: string }>;
  symbol: string;
  location: { path: string; line: number; character: number };
}

export function isDocumentSymbol(symbol: DocumentSymbol | SymbolInformation): symbol is DocumentSymbol {
  return 'children' in symbol || 'selectionRange' in symbol;
}

export function isSymbolInformation(symbol: DocumentSymbol | SymbolInformation): symbol is SymbolInformation {
  return 'location' in symbol && !('children' in symbol);
}

export function isDocumentSymbolArray(symbols: SymbolResponse): symbols is DocumentSymbol[] {
  return Array.isArray(symbols) && symbols.length > 0 && isDocumentSymbol(symbols[0]!);
}

export function formatDocumentSymbols(symbols: DocumentSymbol[], indent: number = 0): string {
  const spaces = '  '.repeat(indent);
  let result = '';

  for (const sym of symbols) {
    result += `${spaces}${sym.name} (${SymbolKind[sym.kind]})\n`;
    if (sym.children && sym.children.length > 0) {
      result += formatDocumentSymbols(sym.children, indent + 1);
    }
  }

  return result;
}

export function formatSymbolInformation(symbols: SymbolInformation[]): string {
  return symbols.map(s => `${s.name} (${SymbolKind[s.kind]}) at ${s.location.uri}`).join('\n');
}

const SymbolKindNames: Record<number, string> = {
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

export function getSymbolKindName(kind: number): string {
  return SymbolKindNames[kind] || 'Unknown';
}

export interface ILSPUtility {
  dispose(): Promise<void>;
}
