import { z } from "zod";
import { resolve } from "node:path";
import type { ITool, ToolContext, LSPDiagnostic, LSPLocation, Hover, CallHierarchyNode, ReferencesGraph, DocumentSymbol, ToolResult } from "@firmius/shared/types";
import { SymbolKind as LSPSymbolKind } from "@firmius/shared";
import { ToolScope } from "@firmius/shared";
import { Engine } from "@firmius/core";

function getAbsolutePath(path: string, context: ToolContext): string {
  const cwd = context.agent.environment.cwd ? context.agent.environment.cwd.toString() : context.host.defaultCwd.toString();
  return resolve(cwd, path);
}

const SymbolKindMap: Record<string, LSPSymbolKind> = {
  'Class': LSPSymbolKind.Class,
  'Function': LSPSymbolKind.Function,
  'Method': LSPSymbolKind.Method,
  'Interface': LSPSymbolKind.Interface,
  'Enum': LSPSymbolKind.Enum,
  'Variable': LSPSymbolKind.Variable,
  'Constant': LSPSymbolKind.Constant
};

// ============================================================================
// 1. LSP_LOOKUP: definition | hover | references
// ============================================================================

const LSPDefinitionSchema = z.object({
  operation: z.literal("definition"),
  path: z.string().describe("Path to the file"),
  line: z.coerce.number().describe("Line number (0-indexed)"),
  character: z.coerce.number().describe("Character position (0-indexed)")
});

const LSPHoverSchema = z.object({
  operation: z.literal("hover"),
  path: z.string().describe("Path to the file"),
  line: z.coerce.number().describe("Line number (0-indexed)"),
  character: z.coerce.number().describe("Character position (0-indexed)")
});

const LSPReferencesLookupSchema = z.object({
  operation: z.literal("references"),
  path: z.string().describe("Path to the file"),
  line: z.coerce.number().describe("Line number (0-indexed)"),
  character: z.coerce.number().describe("Character position (0-indexed)")
});

export const LSPLookupInputSchema = z.discriminatedUnion("operation", [
  LSPDefinitionSchema,
  LSPHoverSchema,
  LSPReferencesLookupSchema
]);

export type LSPLookupInput = z.infer<typeof LSPLookupInputSchema>;
export type LSPLookupOutput = {
  locations?: LSPLocation[];
  hover?: Hover;
};

export const LSPLookupTool: ITool<LSPLookupInput, LSPLookupOutput> = {
  metadata: {
    name: "lsp_lookup",
    description: "Look up definitions, hover info, or references.",
    scope: ToolScope.Semantic
  },
  input: LSPLookupInputSchema,
  execute: async (input: LSPLookupInput, context: ToolContext): Promise<ToolResult<LSPLookupOutput>> => {
    try {
      const absPath = getAbsolutePath(input.path, context);
      const lspUtility = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());

      switch (input.operation) {
        case "definition":
          const locs = await lspUtility.getDefinition(absPath, input.line, input.character);
          return { success: true, summary: `Found ${locs.length} definitions`, output: { locations: locs } };
        case "hover":
          const hover = await lspUtility.getHover(absPath, input.line, input.character);
          return { success: true, summary: hover ? "Retrieved hover info" : "No hover info", output: { hover: hover as Hover } };
        case "references":
          const refs = await lspUtility.getReferences(absPath, input.line, input.character);
          return { success: true, summary: `Found ${refs.length} references`, output: { locations: refs } };
        default:
          return { success: false, summary: "Invalid op", error: "Invalid operation" };
      }
    } catch (e: any) {
      return { success: false, summary: "LSP error", error: e.message };
    }
  },
  summarizeInput: (input: LSPLookupInput) => `lsp_lookup: ${input.operation} on ${input.path}`,
};

// ============================================================================
// 2. LSP_INSPECT: symbols | exports | summary | diagnostics
// ============================================================================

const LSPSymbolsSchema = z.object({
  operation: z.literal("symbols"),
  path: z.string().describe("Path to the file"),
  kinds: z.array(z.enum(['Class', 'Function', 'Method', 'Interface', 'Enum', 'Variable', 'Constant'])).optional().describe("Filter symbols by kind.")
});

const LSPExportsSchema = z.object({
  operation: z.literal("exports"),
  path: z.string().describe("Path to the file")
});

const LSPFileSummarySchema = z.object({
  operation: z.literal("summary"),
  path: z.string().describe("Path to the file")
});

const LSPDiagnosticsSchema = z.object({
  operation: z.literal("diagnostics"),
  path: z.string().describe("Path to the file")
});

export const LSPInspectInputSchema = z.discriminatedUnion("operation", [
  LSPSymbolsSchema,
  LSPExportsSchema,
  LSPFileSummarySchema,
  LSPDiagnosticsSchema
]);

export type LSPInspectInput = z.infer<typeof LSPInspectInputSchema>;

export interface LSPInspectOutput {
  symbols?: DocumentSymbol[];
  exports?: Array<{ name: string; kind: string; line: number }>;
  summary?: {
    imports: string[];
    exports: Array<{ name: string; kind: string }>;
    classes: string[];
    functions: string[];
  };
  diagnostics?: LSPDiagnostic[];
}

export const LSPInspectTool: ITool<LSPInspectInput, LSPInspectOutput> = {
  metadata: {
    name: "lsp_inspect",
    description: "Inspect a file's symbols, exports, summary, or diagnostics.",
    scope: ToolScope.Semantic
  },
  input: LSPInspectInputSchema,
  execute: async (input: LSPInspectInput, context: ToolContext): Promise<ToolResult<LSPInspectOutput>> => {
    try {
      const absPath = getAbsolutePath(input.path, context);
      const lspUtility = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());

      switch (input.operation) {
        case "symbols": {
          const kinds = input.kinds?.map(k => SymbolKindMap[k]).filter((k): k is LSPSymbolKind => k !== undefined);
          const symbols = await lspUtility.getDocumentSymbols(absPath, kinds);
          return { success: true, summary: `Indexed ${symbols.length} symbols`, output: { symbols } };
        }
        case "exports": {
          const exports = await lspUtility.getExports(absPath);
          const mapped = exports.map((e: any) => ({ name: e.name, kind: LSPSymbolKind[e.kind]!, line: e.line }));
          return { success: true, summary: `Found ${mapped.length} exports`, output: { exports: mapped } };
        }
        case "summary": {
          const summaryData = await lspUtility.getFileSummary(absPath);
          const outputSummary = { 
            ...summaryData, 
            exports: summaryData.exports.map((e: any) => ({ name: e.name, kind: LSPSymbolKind[e.kind]! })) 
          };
          return { success: true, summary: "Generated file summary", output: { summary: outputSummary } };
        }
        case "diagnostics": {
          await lspUtility.getClientForFile(absPath);
          const diagnostics = await lspUtility.getDiagnostics(absPath);
          return { success: true, summary: `Found ${diagnostics.length} diagnostics`, output: { diagnostics } };
        }
        default:
          return { success: false, summary: "Invalid op", error: "Invalid operation" };
      }
    } catch (e: any) {
      return { success: false, summary: "LSP error", error: e.message };
    }
  },
  summarizeInput: (input: LSPInspectInput) => `lsp_inspect: ${input.operation} on ${input.path}`,
};

// ============================================================================
// 3. LSP_FIND
// ============================================================================

export interface LSPFindInput {
  name: string;
  kind?: 'Class' | 'Function' | 'Method' | 'Interface' | 'Enum' | 'Variable' | 'Constant';
}

export interface LSPFindOutput {
  definitions: Array<{ file: string; line: number; character: number; name: string }>;
}

export const LSPFindInputSchema = z.object({
  name: z.string().describe("Symbol name to search for."),
  kind: z.enum(['Class', 'Function', 'Method', 'Interface', 'Enum', 'Variable', 'Constant']).optional()
});

export const LSPFindTool: ITool<LSPFindInput, LSPFindOutput> = {
  metadata: {
    name: "lsp_find",
    description: "Find symbol definitions by name.",
    scope: ToolScope.Semantic
  },
  input: LSPFindInputSchema,
  execute: async (input: LSPFindInput, context: ToolContext): Promise<ToolResult<LSPFindOutput>> => {
    try {
      const lspUtility = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());
      const kind = input.kind ? SymbolKindMap[input.kind] : undefined;
      const definitions = await lspUtility.findSymbol(input.name, kind);
      return { success: true, summary: `Found ${definitions.length} definitions for "${input.name}"`, output: { definitions } };
    } catch (e: any) {
      return { success: false, summary: "LSP error", error: e.message };
    }
  },
  summarizeInput: (input: LSPFindInput) => `lsp_find: ${input.name}`,
};

// ============================================================================
// 4. LSP_DEPENDENCIES: hierarchy | callers | graph
// ============================================================================

const LSPHierarchySchema = z.object({
  operation: z.literal("hierarchy"),
  path: z.string().describe("Path to the file"),
  line: z.coerce.number().describe("Line number (0-indexed)"),
  character: z.coerce.number().describe("Character position (0-indexed)"),
  direction: z.enum(['incoming', 'outgoing']).describe("Call hierarchy direction"),
  depth: z.coerce.number().optional().default(2)
});

const LSPCallersSchema = z.object({
  operation: z.literal("callers"),
  name: z.string().describe("Function or method name"),
  file: z.string().optional(),
  depth: z.coerce.number().optional().default(2)
});

const LSPGraphSchema = z.object({
  operation: z.literal("graph"),
  path: z.string().describe("Path to file"),
  line: z.coerce.number().describe("Line number (0-indexed)"),
  character: z.coerce.number().describe("Character position (0-indexed)"),
  maxDepth: z.coerce.number().optional().default(1)
});

export const LSPDependenciesInputSchema = z.discriminatedUnion("operation", [
  LSPHierarchySchema,
  LSPCallersSchema,
  LSPGraphSchema
]);

export type LSPDependenciesInput = z.infer<typeof LSPDependenciesInputSchema>;

export interface LSPDependenciesOutput {
  hierarchy?: CallHierarchyNode[];
  callers?: Array<{ file: string; line: number; symbol: string }>;
  graph?: ReferencesGraph;
}

export const LSPDependenciesTool: ITool<LSPDependenciesInput, LSPDependenciesOutput> = {
  metadata: {
    name: "lsp_dependencies",
    description: "Analyze dependencies.",
    scope: ToolScope.Semantic
  },
  input: LSPDependenciesInputSchema,
  execute: async (input: LSPDependenciesInput, context: ToolContext): Promise<ToolResult<LSPDependenciesOutput>> => {
    try {
      const lspUtility = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());

      switch (input.operation) {
        case "hierarchy": {
          const absPath = getAbsolutePath(input.path, context);
          const hierarchy = await lspUtility.getCallHierarchy(absPath, input.line, input.character, input.direction, input.depth ?? 2);
          return { success: true, summary: `Generated call hierarchy`, output: { hierarchy } };
        }
        case "callers": {
          const callers = await lspUtility.findCallers(input.name, input.file);
          return { success: true, summary: `Found ${callers.length} callers`, output: { callers } };
        }
        case "graph": {
          const absPath = getAbsolutePath(input.path, context);
          const graph = await lspUtility.getReferencesGraph(absPath, input.line, input.character, input.maxDepth ?? 1);
          return { success: true, summary: "Built references graph", output: { graph } };
        }
        default:
          return { success: false, summary: "Invalid op", error: "Invalid operation" };
      }
    } catch (e: any) {
      return { success: false, summary: "LSP error", error: e.message };
    }
  },
  summarizeInput: (input: LSPDependenciesInput) => `lsp_dependencies: ${input.operation}`,
};

// ============================================================================
// 5. LSP_EXPLORE: top_files | navigate
// ============================================================================

const LSPTopFilesSchema = z.object({
  operation: z.literal("top_files"),
  limit: z.coerce.number().optional().default(20)
});

const LSPNavigateSchema = z.object({
  operation: z.literal("navigate"),
  fromFile: z.string().optional(),
  toFile: z.string().optional(),
  depth: z.coerce.number().optional().default(3)
});

export const LSPExploreInputSchema = z.discriminatedUnion("operation", [
  LSPTopFilesSchema,
  LSPNavigateSchema
]);

export type LSPExploreInput = z.infer<typeof LSPExploreInputSchema>;

export interface LSPExploreOutput {
  files?: Array<{ path: string; symbolCount: number; referenceCount: number; score: number; primaryLanguage: string }>;
  path?: Array<{ file: string; imports: string[]; exports: Array<{ name: string; kind: string }> }>;
}

export const LSPExploreTool: ITool<LSPExploreInput, LSPExploreOutput> = {
  metadata: {
    name: "lsp_explore",
    description: "Explore the codebase.",
    scope: ToolScope.Semantic
  },
  input: LSPExploreInputSchema,
  execute: async (input: LSPExploreInput, context: ToolContext): Promise<ToolResult<LSPExploreOutput>> => {
    try {
      const lspUtility = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());

      switch (input.operation) {
        case "top_files": {
          const stats = await lspUtility.scan();
          const files = stats.topFiles.slice(0, input.limit);
          return { success: true, summary: `Found ${files.length} top files`, output: { files } };
        }
        case "navigate": {
          const fromFile = input.fromFile ? getAbsolutePath(input.fromFile, context) : undefined;
          const toFile = input.toFile ? getAbsolutePath(input.toFile, context) : undefined;
          const path = await lspUtility.navigateImports(fromFile, toFile);
          const mappedPath = path.map((p: any) => ({
            file: p.file,
            imports: p.imports,
            exports: p.exports.map((e: any) => ({ name: e.name, kind: LSPSymbolKind[e.kind]! }))
          }));
          return { success: true, summary: `Navigation path has ${mappedPath.length} steps`, output: { path: mappedPath } };
        }
        default:
          return { success: false, summary: "Invalid op", error: "Invalid operation" };
      }
    } catch (e: any) {
      return { success: false, summary: "LSP error", error: e.message };
    }
  },
  summarizeInput: (input: LSPExploreInput) => `lsp_explore: ${input.operation}`,
};

export const AllLSPTools = [ LSPLookupTool, LSPInspectTool, LSPFindTool, LSPDependenciesTool, LSPExploreTool ];
