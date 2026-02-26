import { Agent } from "./Agent";
import { ToolScope } from "@firmius/shared";
import {
  AgentWorkType,
  type IAgent,
  type AgentContext,
  type AgentPermissions,
} from "@firmius/shared";
import { purposeRegistry } from "./registry/PurposeRegistry";
import type {
  IAgentFactory,
  AgentFactorySummonOptions,
} from "@firmius/shared";
import { type HostConfig, type IHost } from "@firmius/shared";
import { randomUUID } from "node:crypto";

import {
  DEFAULT_MODEL_CTX,
  DEFAULT_PROVIDER,
  DEFAULT_MODEL,
  CONTEXT_CRITICAL_THRESHOLD
} from "./Constants";
import { Engine } from "./Engine";
import { logger } from "@firmius/shared";
import { DEFAULT_OBJECTIVE } from "./SystemPrompt";
import UserConfigManager from "./config/UserConfigManager";
import { HostFactory } from "./HostFactory";

export class AgentFactory implements IAgentFactory {
  agents: Map<string, IAgent> = new Map();

  async summon(options: AgentFactorySummonOptions): Promise<IAgent> {
    const id = randomUUID();
    const parentAgent = options.parentId
      ? this.agents.get(options.parentId)
      : undefined;

    if (parentAgent) {
      const provider =
        Engine.providers[
        parentAgent.execution.generationOptions.providerId
        ];
      const modelInfo = provider
        ?.listModels()
        .find(
          (m) =>
            m.name === parentAgent.execution.generationOptions.modelId,
        );
      const modelCtx = modelInfo?.ctx ?? DEFAULT_MODEL_CTX;
      const usagePercent =
        (parentAgent.state.metrics.totalTokens / modelCtx) * 100;

      if (usagePercent > CONTEXT_CRITICAL_THRESHOLD * 100 && !options.disableCompaction) {
        throw new Error(
          `Delegation blocked: Parent agent is at ${usagePercent.toFixed(1)}% context capacity. Resolve current state first.`,
        );
      }

      // Hierarchy validation: check if parent can spawn this purpose
      const parentPurpose = parentAgent.identity.purpose;
      const parentDef = purposeRegistry.getPurpose(parentPurpose);
      const childPurpose = options.purpose;

      if (parentDef?.canSpawn && !parentDef.canSpawn.includes(childPurpose)) {
        throw new Error(
          `Hierarchy violation: ${parentPurpose} agents cannot spawn ${childPurpose} agents. Allowed: ${parentDef.canSpawn.join(", ")}`
        );
      }
    }

    // Resolve generation options: explicit > parent > defaults
    let providerId: string;
    let modelId: string;
    if (
      options.generationOptions?.providerId &&
      options.generationOptions?.modelId
    ) {
      providerId = options.generationOptions.providerId;
      modelId = options.generationOptions.modelId;
    } else if (parentAgent) {
      providerId =
        options.generationOptions?.providerId ??
        parentAgent.execution.generationOptions.providerId;
      modelId =
        options.generationOptions?.modelId ??
        parentAgent.execution.generationOptions.modelId;
    } else {
      // Check user config for purpose-specific defaults
      const userConfigManager = UserConfigManager.getInstance();
      await userConfigManager.refresh(); // ensure we have latest from disk
      const userConfig = userConfigManager.get();
      const purpose = options.purpose;
      const override = userConfig.defaultModels[purpose];
      if (override && override.providerId && override.modelId) {
        providerId = override.providerId;
        modelId = override.modelId;
      } else {
        providerId = options.generationOptions?.providerId ?? DEFAULT_PROVIDER;
        modelId = options.generationOptions?.modelId ?? DEFAULT_MODEL;
      }
    }

    const provider = Engine.providers[providerId];
    if (!provider) throw new Error(`Provider ${providerId} not found`);

    const modelInfo = provider.listModels().find((m) => m.name === modelId);
    const modelCtx = modelInfo?.ctx ?? DEFAULT_MODEL_CTX;

    // Host resolution
    let host: IHost;
    if (options.host === "inherit") {
      if (!options.parentId)
        throw new Error("Cannot inherit host without parentId");
      const parent = this.agents.get(options.parentId);
      if (!parent)
        throw new Error(`Parent agent ${options.parentId} not found`);
      host = parent.environment.host;
    } else if (options.host && typeof options.host === "object" && "defaultCwd" in options.host) {
      host = options.host as IHost;
    } else {
      host = await this.createHost(options.host as HostConfig);
      await host.init();
    }
    // Permission intersection
    const purposeDef = purposeRegistry.getPurpose(options.purpose);
    const defaultScopes = purposeDef?.scopes as ToolScope[] || [ToolScope.FilesystemRead, ToolScope.Delegation, ToolScope.Todo];
    let finalScopes = defaultScopes;

    if (options.parentId) {
      const parent = this.agents.get(options.parentId);
      if (parent && parent.environment.permissions?.scopes) {
        finalScopes = defaultScopes.filter((s) =>
          parent.environment.permissions.scopes.includes(s),
        );
      }
    }

    const permissions: AgentPermissions = {
      scopes: finalScopes,
      allowOutsideCwd: options.constraints?.allowOutsideCwd ?? false,
      allowPaths: options.allowPaths,
    };

    let cwd: string;
    if (options.cwd) {
      cwd = options.cwd;
    } else if (options.host === "inherit" && parentAgent) {
      cwd = parentAgent.environment.cwd.toString();
    } else {
      cwd = host.defaultCwd.toString();
    }

    // Dynamic Context Budgeting
    // We allocate ~25% of the total context for Watched Files.
    // This increased budget accommodates the REQUIREMENT that files must be fully
    // allocated in context before editing (see EDIT CONSTRAINT in system prompt).
    // 25% of 200k tokens = 50k tokens.
    // Assuming 1 token ~= 4 chars, that's ~200k chars for full file content.
    const maxContextChars = Math.floor(modelCtx * 0.25 * 4);

    const threadId =
      options.threadId ??
      (parentAgent ? parentAgent.identity.threadId : id);

    const isConversational =
      (options.workType ?? AgentWorkType.Goal) === AgentWorkType.Conversational;

    const context: AgentContext = {
      identity: {
        id,
        threadId,
        readableName: options.readableName,
        parentId: options.parentId,
        subagentIds: [],
        purpose: options.purpose,
        objective: options.objective || DEFAULT_OBJECTIVE,
        purposeDefinition: purposeDef?.systemPrompt,
      },
      historyData: {
        history: isConversational
          ? {
            type: AgentWorkType.Conversational,
            conversation: { history: [] },
          }
          : {
            type: AgentWorkType.Goal,
            workflow: {
              turns: [],
              timestamp: Date.now(),
              completed: false,
              finalMessage: null,
            },
          },
        reasoningHistory: [],
        reasoningHistoryLimit: 2,
      },
      environment: {
        host,
        cwd,
        permissions,
        injectedFiles: options.injectedFiles,
        attachedFiles: [],
      },
      state: {
        status: "idle",
        metrics: {
          totalTokens: 0,
          lastTurnTokens: 0,
          lastPromptTokens: 0,
          startTime: Date.now(),
        },
        todos: [],
        nextTodoId: 1,
        ownedProcesses: [],
      },
      execution: {
        generationOptions: {
          providerId,
          modelId,
        },
        maxContextChars,
        tags: options.tags || {},
        disableCompaction: options.disableCompaction || false,
        anchors: new Set(),
        injectedContext: options.additionalContext?.injectedContext,
      },
      io: {
        onTurn: options.onTurn,
      },
    };

    const agent = new Agent(context);
    this.agents.set(id, agent);

    if (options.parentId) {
      const parent = this.agents.get(options.parentId);
      if (parent) {
        parent.identity.subagentIds.push(id);
      }
    }

    return agent;
  }

  async terminate(id: string): Promise<void> {
    const agent = this.agents.get(id);
    if (!agent) return;

    // 1. Recursively terminate children first (Bottom-up cleanup)
    const children = Array.from(this.agents.values()).filter(
      (a) => a.identity.parentId === id,
    );
    for (const child of children) {
      await this.terminate(child.id);
    }

    // 2. Reap owned processes
    logger.info(
      `[AgentFactory] Reaping ${agent.state.ownedProcesses.length} processes for agent ${id}...`,
    );
    for (const pid of agent.state.ownedProcesses) {
      try {
        const handle = Engine.processManager.get(pid);
        if (handle) {
          await handle.kill("SIGKILL");
          Engine.processManager.unregister(pid);
        }
      } catch (e) {
        logger.error(
          `[AgentFactory] Failed to kill process ${pid}: ${e instanceof Error ? e.message : String(e)}`,
        );
      }
    }

    // 3. Shutdown LSPUtility resources for this agent's host
    try {
      const hostKey = agent.environment.host;
      const cwdKey = agent.environment.cwd.toString();

      // Check if we have an LSPUtility for this host/cwd
      const hostMap = Engine.lspUtilities.get(hostKey);
      if (hostMap && hostMap.has(cwdKey)) {
        const utility = hostMap.get(cwdKey);
        if (utility) {
          logger.info(`[AgentFactory] Disposing LSPUtility for ${cwdKey}...`);
          await utility.dispose();
          hostMap.delete(cwdKey);
        }
      }

      // Clean up empty host entries
      if (hostMap && hostMap.size === 0) {
        Engine.lspUtilities.delete(hostKey);
      }
    } catch (e) {
      logger.error(
        `[AgentFactory] Error disposing resources: ${e instanceof Error ? e.message : String(e)}`,
      );
    }

    // 4. Remove from registry
    this.agents.delete(id);
    logger.info(
      `[AgentFactory] Agent ${id} terminated and resources cleaned up.`,
    );
  }

  private async createHost(config: HostConfig): Promise<IHost> {
    return HostFactory.create(config);
  }
}
