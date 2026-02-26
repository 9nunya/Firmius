import { z } from "zod";
import type { ITool, ToolContext, AgentActResult, AgentPurpose, ToolResult } from "@firmius/shared/types";
import { AgentWorkType, HostType } from "@firmius/shared";
import { ToolScope } from "@firmius/shared";
import { Engine } from "@firmius/core";
import UserConfigManager from "@firmius/core/config";
import { purposeRegistry } from "@firmius/core/registry";

// =============================================================================
// TYPES
// =============================================================================

export interface AgentDelegateInput {
  title: string;
  objective: string;
  purpose: AgentPurpose;
  agentId: string;
  blocking?: boolean;
  host?: "inherit" | { type: HostType; options?: any };
  cwd?: string;
  allowOutsideCwd?: boolean;
  allowPaths?: string[];
  injectedFiles?: Array<{ path: string; content: string }>;
}

export interface AgentDelegateOutput {
  agentId: string;
  status: "completed" | "spawned";
  result?: string;
}

// =============================================================================
// SCHEMA
// =============================================================================

const HostConfigSchema = z.preprocess(
  (val) => {
    if (typeof val === "string") {
      if (val === "inherit") return val;
      try {
        const parsed = JSON.parse(val);
        // If it's just a number string like "0", JSON.parse returns a number
        if (typeof parsed === "number") return { type: parsed };
        return parsed;
      } catch {
        const num = parseInt(val, 10);
        if (!isNaN(num)) return { type: num };
        return val;
      }
    }
    if (typeof val === "number") {
      return { type: val };
    }
    return val;
  },
  z.union([
    z
      .literal("inherit")
      .describe("Share your current environment (host, CWD, and permissions)."),
    z
      .object({
        type: z
          .nativeEnum(HostType)
          .describe("Target host type (0:Local, 1:Docker, 2:SSH)."),
        options: z.any().optional(),
      })
      .describe("Execute on a specific host type."),
    z
      .object({
        type: z
          .literal(HostType.Docker)
          .describe("Docker container environment."),
        options: z.object({
          image: z
            .string()
            .optional()
            .describe("Docker image (default: 'firmius-sandbox:latest')."),
          containerName: z
            .string()
            .optional()
            .describe("Optional name for the container."),
          env: z
            .record(z.string(), z.string())
            .optional()
            .describe("Environment variables for the container."),
          repo: z
            .string()
            .url()
            .optional()
            .describe(
              "GitHub repository URL to clone into the container's root.",
            ),
        }),
      })
      .describe("Execute inside a sandboxed Docker container."),
    z
      .object({
        type: z.literal(HostType.RemoteSSH).describe("Remote SSH environment."),
        options: z.object({
          host: z.string().describe("Remote hostname or IP."),
          port: z.number().optional().default(22).describe("SSH port."),
          username: z.string().describe("SSH username."),
          password: z.string().optional().describe("SSH password."),
          privateKeyPath: z
            .string()
            .optional()
            .describe("Path to private key."),
        }),
      })
      .describe("Execute on a remote machine via SSH."),
  ]),
);

const AgentDelegateInputSchema = z.object({
  title: z
    .string()
    .min(5, "Title is too short. Describe the task briefly.")
    .describe("A user-friendly title describing the sub-agent's task."),
  objective: z
    .string()
    .min(10, "Objective is too short. Be descriptive.")
    .describe("The specific task for the sub-agent."),
  purpose: z
    .string()
    .describe(
      "The persona of the agent (general, coder, researcher, compactor, orchestrator, architect, executor, verifier, mapper).",
    ),
  agentId: z
    .string()
    .min(3, "Agent ID must be at least 3 characters")
    .regex(
      /^[a-z0-9-]+$/,
      "Agent ID must be lowercase alphanumeric with hyphens only",
    )
    .describe(
      "A unique identifier for this agent (e.g., 'research-auth', 'fix-tests').",
    ),

  blocking: z
    .coerce.boolean()
    .default(true)
    .describe(
      "If true (default), blocks until the agent completes and returns its result. If false, spawns the agent in the background and returns immediately — call again with the same agentId to collect its result.",
    ),
  host: HostConfigSchema.default("inherit").describe("Host configuration."),
  cwd: z.string().optional().describe("Working directory for the sub-agent."),
  allowOutsideCwd: z
    .boolean()
    .default(false)
    .describe("Allow sub-agent to access files outside its CWD."),
  allowPaths: z
    .array(z.string())
    .optional()
    .describe(
      "Glob patterns restricting which paths the sub-agent can write to.",
    ),
  injectedFiles: z
    .array(
      z.object({
        path: z.string(),
        content: z.string(),
      }),
    )
    .optional()
    .describe("Files to pre-load into the sub-agent's context."),
});

export { AgentDelegateInputSchema };

// =============================================================================
// PENDING AGENTS STORE (for non-blocking mode)
// =============================================================================

const pendingAgents: Map<
  string,
  {
    agentId: string;
    customId: string;
    promise: Promise<AgentActResult[]>;
    status: "running" | "completed" | "failed";
    result?: AgentActResult[];
    error?: Error;
    agent?: any;
  }
> = new Map();

// =============================================================================
// AGENT DELEGATE TOOL
// =============================================================================

const resolvePurpose = (input: AgentDelegateInput): AgentPurpose => {
  const direct = purposeRegistry.getPurpose(input.purpose);
  if (direct) return input.purpose;

  const fromAgentId = purposeRegistry.getPurpose(input.agentId);
  if (fromAgentId) return input.agentId as AgentPurpose;

  const normalized = input.purpose
    .toLowerCase()
    .replace(/[^a-z0-9-]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "");
  const normalizedPurpose = purposeRegistry.getPurpose(normalized);
  if (normalizedPurpose) return normalized as AgentPurpose;

  return input.purpose;
};

const spawnAgent = async (input: AgentDelegateInput, context: ToolContext) => {
  const coordinator = context.coordinator;
  const threadId = context.agent.identity.threadId;
  const thread = Engine.getThread(threadId);
  if (!thread) {
    throw new Error(`Thread ${threadId} not found`);
  }

  const parentAgent = context.agent;
  const resolvedPurpose = resolvePurpose(input);

  // Apply user config model overrides based on purpose
  const userConfigManager = UserConfigManager.getInstance();
  await userConfigManager.refresh();
  const userConfig = userConfigManager.get();
  const purpose = resolvedPurpose;
  const override = userConfig.defaultModels[purpose];
  let effectiveGenerationOptions = parentAgent.execution.generationOptions;
  if (override && override.providerId && override.modelId) {
    effectiveGenerationOptions = {
      ...effectiveGenerationOptions,
      providerId: override.providerId,
      modelId: override.modelId,
    };
  }

  // Summon the agent instance
  const finalAllowOutside = context.agent.environment.permissions
    .allowOutsideCwd
    ? input.allowOutsideCwd
    : false;
  const agent = await Engine.agentFactory.summon({
    purpose: resolvedPurpose,
    objective: input.objective,
    readableName: input.agentId,
    cwd: (input.cwd ?? context.agent.environment.cwd) as string,
    host: input.host as any,
    workType: AgentWorkType.Goal,
    threadId,
    parentId: parentAgent.identity.id,
    generationOptions: effectiveGenerationOptions,
    constraints: {
      allowOutsideCwd: finalAllowOutside,
    },
    allowPaths: input.allowPaths,
    injectedFiles: input.injectedFiles,
  });

  // Register agent in fleet registry using the actual agent id
  await coordinator.fleet.registerAgent({
    id: agent.id,
    purpose: resolvedPurpose,
    parentId: parentAgent.identity.id,
  });

  // Register agent in thread's agentRegistry
  const threadAgents = (thread as any).agentRegistry || (thread as any).agents;
  if (threadAgents && typeof threadAgents.set === "function") {
    threadAgents.set(agent.id, agent);
  }

  return agent;
};

const extractResult = (results: AgentActResult[]): string => {
  const lastResponse = results[results.length - 1];
  const content = lastResponse?.response?.content;
  if (typeof content === "string") return content;
  if (Array.isArray(content)) return JSON.stringify(content);
  return "Sub-agent completed with no output.";
};

const awaitPending = async (
  fullCustomId: string,
  agentId: string,
): Promise<ToolResult<AgentDelegateOutput>> => {
  const pending = pendingAgents.get(fullCustomId);

  if (!pending) {
    return {
      success: false,
      summary: "Agent not found",
      error: `No background agent with ID "${agentId}".`,
    };
  }

  if (pending.status === "completed" && pending.result) {
    pendingAgents.delete(fullCustomId);
    return {
      success: true,
      summary: `Agent "${agentId}" completed.`,
      output: {
        agentId,
        status: "completed",
        result: extractResult(pending.result),
      },
    };
  }

  if (pending.status === "failed") {
    const error = pending.error;
    pendingAgents.delete(fullCustomId);
    return {
      success: false,
      summary: `Agent "${agentId}" failed`,
      error: `Agent failed: ${error?.message || "Unknown error"}`,
    };
  }

  // Still running — wait for completion
  try {
    const results = await pending.promise;
    pendingAgents.delete(fullCustomId);

    return {
      success: true,
      summary: `Agent "${agentId}" completed.`,
      output: {
        agentId,
        status: "completed",
        result: extractResult(results),
      },
    };
  } catch (e: any) {
    pendingAgents.delete(fullCustomId);
    return {
      success: false,
      summary: `Agent "${agentId}" failed`,
      error: `Agent failed: ${e.message}`,
    };
  }
};

export const AgentDelegateTool: ITool<AgentDelegateInput, AgentDelegateOutput> =
{
  metadata: {
    name: "agent_delegate",
    description: `Delegate a task to a specialized sub-agent. Supports blocking and background modes.`,
    scope: ToolScope.Delegation,
  },
  input: AgentDelegateInputSchema,
  execute: async (
    input: AgentDelegateInput,
    context: ToolContext,
  ): Promise<ToolResult<AgentDelegateOutput>> => {
    const fullCustomId = `${context.agent.identity.id}:${input.agentId}`;

    // If this agentId already exists as a pending bg agent, collect its result
    if (pendingAgents.has(fullCustomId)) {
      return awaitPending(fullCustomId, input.agentId);
    }

    try {
      const agent = await spawnAgent(input, context);

      // Update tool call metadata immediately so the UI knows we've spawned an agent
      if (context.threadId && context.toolCallId) {
        Engine.emitToolCallUpdate(context.threadId, {
          agentId: context.agent.id,
          callId: context.toolCallId,
          metadata: { spawnedAgentId: agent.id },
          summary: `Delegating task to ${input.agentId}...`
        });
      }

      if (input.blocking) {
        try {
          const results = await agent.actUntilAgentEnds();

          return {
            success: true,
            summary: `Agent "${input.agentId}" completed.`,
            output: {
              agentId: input.agentId,
              status: "completed",
              result: extractResult(results),
            },
          };
        } catch (e: any) {
          throw e;
        }
      }

      // Non-blocking: fire and forget, store the promise
      const executionPromise = agent.actUntilAgentEnds();

      pendingAgents.set(fullCustomId, {
        agentId: agent.id,
        customId: input.agentId,
        promise: executionPromise,
        status: "running",
        agent: agent,
      });

      executionPromise
        .then((results: AgentActResult[]) => {
          const pending = pendingAgents.get(fullCustomId);
          if (pending) {
            pending.status = "completed";
            pending.result = results;
          }
        })
        .catch((error: Error) => {
          const pending = pendingAgents.get(fullCustomId);
          if (pending) {
            pending.status = "failed";
            pending.error = error;
          }
        });

      return {
        success: true,
        summary: `Spawned "${input.agentId}" in background.`,
        output: {
          agentId: input.agentId,
          status: "spawned",
        },
        metadata: { spawnedAgentId: agent.id }
      };
    } catch (e: any) {
      return {
        success: false,
        summary: `Failed to delegate to ${input.agentId}`,
        error: e.message,
      };
    }
  },
  summarizeInput: (input: AgentDelegateInput) => {
    const mode = input.blocking ? "blocking" : "bg";
    return `delegate [${input.agentId}] (${mode}): ${input.title}`;
  },
  summary: (output: ToolResult<AgentDelegateOutput>) => {
    return output.summary;
  },
};

export const AllDelegationTools = [AgentDelegateTool];
