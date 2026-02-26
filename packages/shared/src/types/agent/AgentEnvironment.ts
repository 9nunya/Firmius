import type { ToolScope } from ".";

export type { ToolScope } from "../tool/ITool";

export interface AgentPermissions {
  scopes: ToolScope[];
  allowOutsideCwd: boolean;
  allowPaths?: string[];
}

export interface AgentEnvironment {
  host: import("../host/IHost").IHost;
  cwd: import("bun").PathLike;
  permissions: AgentPermissions;
  injectedFiles?: Array<{
    path: string;
    content: string;
  }>;
  attachedFiles?: string[];
}
