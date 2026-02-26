import type { ZodType } from "zod";
import type { IAgent } from "../agent";
import type { IHost } from "../host/IHost";
import type { ProviderTool } from "../provider/IProvider";
import type { ICoordinator } from "../coordinator/ICoordinator";

export enum ToolScope {
  FilesystemRead = "fs:read",
  FilesystemWrite = "fs:write",
  Process = "proc",
  Semantic = "lsp",
  Delegation = "delegation",
  Compaction = "compaction",
  Web = "web",
  Todo = "todo",
  Git = "git",
  Orchestration = "orchestration",
  Worker = "worker",
}

/**
 * Standardized result for all tool executions.
 * This ensures the UI and internal history can handle any tool uniformly.
 */
export interface ToolResult<O = any> {
  /** Logical success of the tool operation. */
  success: boolean;
  /** Brief human-readable summary of what happened. */
  summary: string;
  /** The typed data payload for the frontend/history. */
  output?: O;
  /** Detail error message if success is false. */
  error?: string;
  /** Calculated by the engine or host. */
  durationMs?: number;
  /** Mid-run telemetry (e.g., spawnedAgentId, processPid). */
  metadata?: Record<string, any>;
}

export interface ToolMetadata {
  name: string;
  description: string;
  scope: ToolScope;
}

export interface ToolContext {
  host: IHost;
  agent: IAgent;
  coordinator: ICoordinator;
  /** Optional tracking info for snapshot/change logging */
  toolCallId?: string;
  turnIndex?: number;
  threadId?: string;
}

export interface ITool<I = any, O = any> {
  input: ZodType<I>;
  metadata: ToolMetadata;
  summarizeInput(input: I): string;
  /**
   * Execute the tool logic.
   * MUST always return a ToolResult, never throw.
   */
  execute(input: I, context: ToolContext): Promise<ToolResult<O>>;
  /**
   * Legacy summary generator.
   * New tools should prefer setting ToolResult.summary during execute.
   */
  summary?(output: ToolResult<O>, input: I): string;
}

export const toolToProviderTool = (tool: ITool<unknown, unknown>): ProviderTool => {
  const zodSchema = tool.input as unknown as { toJSONSchema(): object };
  const inputSchema = zodSchema.toJSONSchema();
  delete (inputSchema as Record<string, unknown>).$schema;
  delete (inputSchema as Record<string, unknown>).additionalProperties;

  return {
    inputSchema,
    name: tool.metadata.name,
    description: tool.metadata.description,
  };
};

export type { ProviderTool };
