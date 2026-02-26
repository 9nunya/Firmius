export type AgentPurpose = string;

export const BuiltinPurposes = {
  General: "general",
  Coder: "coder",
  Researcher: "researcher",
  Compactor: "compactor",
  Orchestrator: "orchestrator",
  Architect: "architect",
  Executor: "executor",
  Verifier: "verifier",
  Mapper: "mapper",
} as const;

export interface AgentIdentity {
  id: string;
  threadId: string;
  parentId?: string;
  subagentIds: string[];
  purpose: AgentPurpose;
  objective: string;
  readableName?: string;
  purposeDefinition?: string;
}
