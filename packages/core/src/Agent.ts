import {
  type AgentActResult,
  AgentActType,
  type AgentContext,
  AgentWorkType,
  type AgentTurn,
  type IAgent,
  type ToolResult,
  type ProviderToolCall,
  isAgentWorkflow,
  isAgentConversationalMessage,
  BuiltinPurposes,
  logger,
  type AgentHistory,
  type AgentConversationalMessage as ACM,
  toolToProviderTool,
  type ICoordinator,
  type IProvider,
} from "@firmius/shared";
import { 
  DIM, RESET, CONTENT, TOOL_START, THINKING, ERROR 
} from "@firmius/shared/utils/term-codes";
import { 
  Engine, 
  CONTEXT_CRITICAL_THRESHOLD, 
  DEFAULT_MODEL_CTX,
  CONTEXT_COMPACTION_THRESHOLD,
} from "./index";
import { BudgetTracker } from "./budget/BudgetTracker";
import { ProtectedContext } from "./budget/ProtectedContext";
import { RollingResultBuffer } from "./budget/RollingResultBuffer";
import { ParallelCompactor } from "./budget/ParallelCompactor";
import { ContextBuilder } from "./ContextBuilder";
import { performance } from "perf_hooks";
import type { ModelInfo, GenerationOptions, ProviderTool } from "@firmius/shared/types";
import { FileWatcher } from "./FileWatcher";
import { FileWatchBudget } from "./budget/FileWatchBudget";
import type { CheckpointData, BufferEntry, EntryPriority } from "./budget/Types";

const DONE_MARKER = ">>>DONE<<<";

export class Agent implements IAgent {
  public id: string;
  get identity() {
    return this.context.identity;
  }
  get environment() {
    return this.context.environment;
  }
  get state() {
    return this.context.state;
  }
  get io() {
    return this.context.io;
  }
  get execution() {
    return this.context.execution;
  }
  get historyData() {
    return this.context.historyData;
  }
  private isCompacting: boolean = false;
  private turnCounter = 0;
  private consecutiveSameToolCallKey?: string;
  private consecutiveSameToolCallCount = 0;
  public readableName: string;

  private fileWatcher: FileWatcher;
  private abortController: AbortController = new AbortController();
  public status: "initializing" | "working" | "completed" | "idle" = "initializing";
  private isExecutingLoop = false;

  private budgetTracker: BudgetTracker;
  private rollingBuffer: RollingResultBuffer;
  private fileWatchBudget: FileWatchBudget;
  // @ts-ignore
  private protectedContext: ProtectedContext;
  // @ts-ignore
  private parallelCompactor: ParallelCompactor;
  private checkpoints: CheckpointData[] = [];

  private currentTurnState?: {
    toolCalls: ProviderToolCall[];
    toolResults: any[];
    reasoning: string;
    content: string;
    pendingToolIds: Set<string>;
  };

  constructor(public context: AgentContext) {
    this.id = context.identity.id;
    context.environment.host.defaultCwd = context.environment.cwd;
    this.readableName =
      context.identity.readableName ||
      `${context.identity.purpose}-${this.id.slice(0, 3)}`;
    
    this.fileWatcher = new FileWatcher();
    this.setupFileWatcher();

    const modelInfo = this.getModelInfo();
    const modelCtx = modelInfo?.ctx ?? DEFAULT_MODEL_CTX;

    this.budgetTracker = new BudgetTracker(modelCtx);
    this.rollingBuffer = new RollingResultBuffer(10, Math.floor(modelCtx * 0.4));
    this.fileWatchBudget = new FileWatchBudget(Math.floor(modelCtx * 0.25 * 4));
    this.protectedContext = new ProtectedContext();
    this.parallelCompactor = new ParallelCompactor();

    // Set provider for compactor
    const provider = this.fetchProvider();
    if (provider) {
      // @ts-ignore
      this.parallelCompactor.setProvider(provider, context.execution.generationOptions.modelId);
    }

    if (context.identity.objective) {
      this.protectedContext.setObjective(context.identity.objective);
    }

    if (context.state.checkpoints) {
      this.checkpoints = context.state.checkpoints;
      this.parallelCompactor.restoreState(this.checkpoints, this.checkpoints.length);
    }

    // Restore callbacks
    context.io.watchFile = (path: string) => this.fileWatcher.watch(path);
    context.io.unwatchFile = (path: string) => this.fileWatcher.unwatch(path);
    
    this.watchExistingFiles();
    this.turnCounter = this.computeInitialTurnCount();

    Engine.emitAgentSpawned(this.context.identity.threadId, {
      agentId: this.id,
      readableName: this.readableName,
      purpose: this.context.identity.purpose,
      parentId: this.context.identity.parentId,
    });

    this.setStatus("idle");
  }

  private setupFileWatcher(): void {
    this.fileWatcher.on("fileChanged", async (filePath: string) => {
      await this.handleExternalFileChange(filePath);
    });

    this.fileWatcher.on("fileDeleted", (filePath: string) => {
      logger.debug(`[${this.readableName}] File deleted externally: ${filePath}`);
      this.fileWatcher.unwatch(filePath);
      this.fileWatchBudget.unwatch(filePath);
    });

    this.fileWatcher.on("error", (filePath: string, error: Error) => {
      if (error.message.includes("ENOENT") && this.fileWatcher.isWatching(filePath)) {
        logger.debug(`[${this.readableName}] Transient file not found for watch: ${filePath}`);
      } else {
        logger.warn(`[${this.readableName}] File watcher error for ${filePath}: ${error.message}`);
      }
    });
  }

  private watchExistingFiles(): void {
    const attached = this.context.environment.attachedFiles || [];
    for (const file of attached as any[]) {
      this.fileWatcher.watch(file.path);
      this.context.environment.host.readFile(file.path)
        .then((content: string) => {
          this.fileWatchBudget.watch(file.path, content, file.offset, file.limit);
        })
        .catch((e: any) => {
          logger.warn(`[${this.readableName}] Failed to load existing file ${file.path}: ${e}`);
        });
    }
  }

  private async handleExternalFileChange(filePath: string): Promise<void> {
    try {
      const entry = this.fileWatchBudget.getEntry(filePath);
      if (!entry) return;

      const stat = await this.context.environment.host.stat(filePath);
      if (stat.mtime > entry.mtime) {
        const content = await this.context.environment.host.readFile(filePath);
        const updated = this.fileWatchBudget.updateContent(filePath, content);

        if (updated) {
          Engine.emitAgentFileChanged(this.context.identity.threadId, {
            agentId: this.id,
            readableName: this.readableName,
            filePath: filePath.toString(),
          });
        } else {
          logger.warn(`[${this.readableName}] Cannot refresh ${filePath}: would exceed file budget`);
        }
      }
    } catch (e: any) {
      logger.warn(`[${this.readableName}] Failed to handle external file change for ${filePath}: ${e.message}`);
    }
  }

  private async refreshWatchedFiles(): Promise<void> {
    for (const entry of this.fileWatchBudget.getAllEntries()) {
      try {
        const stat = await this.context.environment.host.stat(entry.path);
        if (stat.mtime > entry.mtime) {
          const content = await this.context.environment.host.readFile(entry.path);
          const updated = this.fileWatchBudget.updateContent(entry.path, content);
          if (updated) {
            logger.info(`[${this.readableName}] Live refresh: ${entry.path} (modified externally)`);
          }
        }
      } catch (e: any) {
        logger.warn(`[${this.readableName}] Failed to refresh file ${entry.path}: ${e.message}`);
      }
    }
  }

  fetchProvider(): IProvider {
    return Engine.providers[
      this.context.execution.generationOptions.providerId
    ]!;
  }

  getModelInfo(): ModelInfo | undefined {
    const provider = this.fetchProvider();
    const models = provider.listModels ? provider.listModels() : [];
    return models.find((m: any) => m.name === this.context.execution.generationOptions.modelId);
  }

  getTurnCount(): number {
    return this.turnCounter;
  }

  private computeInitialTurnCount(): number {
    const history = this.context.historyData.history;
    if (history.type === AgentWorkType.Goal) {
      const wf = history.workflow;
      if (!wf) return 0;
      let count = wf.turns.length;
      if (wf.finalMessage) count++;
      return count;
    } else {
      const conv = history.conversation;
      if (!conv) return 0;
      let count = 0;
      for (const entry of conv.history) {
        if (isAgentWorkflow(entry)) {
          count += entry.turns.length;
          if (entry.finalMessage) count++;
        } else if (isAgentConversationalMessage(entry) && !entry.isUser) {
          count++;
        }
      }
      return count;
    }
  }

  private setStatus(status: "initializing" | "working" | "completed" | "idle") {
    this.status = status;
    const threadId = this.context.identity.threadId;
    if (threadId) {
      Engine.emitAgentStatus(threadId, {
        agentId: this.id,
        status: status === "completed" ? "idle" : status,
      });
    }
  }

  async act(): Promise<AgentActResult> {
    this.abortController = new AbortController();

    this.currentTurnState = {
      toolCalls: [],
      toolResults: [],
      reasoning: "",
      content: "",
      pendingToolIds: new Set(),
    };

    this.setStatus("working");

    try {
      await this.refreshWatchedFiles();
      this.turnCounter++;
      this.checkContextLimits();
      await this.checkCompaction();
      
      const estimated = this.getContextUsagePercent();
      if (
        this.context.identity.purpose !== BuiltinPurposes.Compactor &&
        estimated >= CONTEXT_CRITICAL_THRESHOLD * 100
      ) {
        throw new Error(
          `Context overflow imminent. Estimated usage will be ${estimated.toFixed(1)}%. Compact history first.`,
        );
      }

      const provider = this.fetchProvider();
      let accumulatedReasoning = "";
      let accumulatedContent = "";
      let toolCalls: Record<string, { call: ProviderToolCall; result: any }> = {};
      const pendingPromises: Promise<void>[] = [];

      this.consecutiveSameToolCallKey = undefined;
      this.consecutiveSameToolCallCount = 0;

      const providerMessages = await ContextBuilder.context2ProviderMessages(
        this.context,
        this.budgetTracker,
        this.checkpoints,
      );

      const providerTools = this.getProviderTools();

      if (Engine.prettyPrint) {
        process.stdout.write(
          `\n${DIM}[${this.readableName}/T${this.turnCounter}] → Provider request: ${providerMessages.length} messages, ${providerTools.length} tools${RESET}\n`,
        );
      }

      try {
        for await (const event of provider.stream(providerMessages, {
          model: this.context.execution.generationOptions.modelId,
          tools: providerTools,
          thinking: true,
          reasoningEffort: this.context.execution.generationOptions.reasoningEffort,
          max_tokens: this.context.execution.generationOptions.maxTokens,
          signal: this.abortController.signal,
        })) {
          if (this.abortController.signal.aborted) break;

          switch (event.type) {
            case "reasoning":
              if (event.text.trim() === "") break;
              accumulatedReasoning += event.text;
              if (this.currentTurnState) this.currentTurnState.reasoning = accumulatedReasoning;
              
              Engine.emitAgentThinking(this.context.identity.threadId, {
                agentId: this.id,
                readableName: this.readableName,
                content: event.text,
                turnCount: this.turnCounter,
              });
              if (Engine.prettyPrint) process.stdout.write(DIM + event.text + RESET);
              break;
            case "content":
              if (event.text.trim() === "") break;
              accumulatedContent += event.text;
              if (this.currentTurnState) this.currentTurnState.content = accumulatedContent;
              
              Engine.emitAgentContent(this.context.identity.threadId, {
                agentId: this.id,
                readableName: this.readableName,
                content: event.text,
                isComplete: false,
                turnCount: this.turnCounter,
              });
              if (Engine.prettyPrint) process.stdout.write(event.text);
              break;
            case "tool_call": {
              const call = typeof event.call === "string" ? JSON.parse(event.call) : event.call;
              const tool = Engine.tools[call.name];
              if (!tool) break;

              const callKey = `${call.name}:${JSON.stringify(call.args)}`;
              if (this.consecutiveSameToolCallKey === callKey) {
                this.consecutiveSameToolCallCount++;
                if (this.consecutiveSameToolCallCount >= 3) {
                  const blockedResult = { success: false, summary: "Tool call loop blocked", error: "Repeated tool call detected." };
                  toolCalls[call.id!] = { call, result: blockedResult };
                  break;
                }
              } else {
                this.consecutiveSameToolCallKey = callKey;
                this.consecutiveSameToolCallCount = 1;
              }

              Engine.emitToolCallStart(this.context.identity.threadId, {
                agentId: this.id,
                readableName: this.readableName,
                toolName: call.name,
                callId: call.id!,
                arguments: call.args as unknown as Record<string, unknown>,
                summary: Engine.tools[call.name]?.summarizeInput?.(call.args) || call.name,
                turnCount: this.turnCounter,
              });

              if (Engine.prettyPrint) {
                process.stdout.write(
                  `\n${TOOL_START}[${this.readableName}/T${this.turnCounter} ${CONTENT}CALL${RESET}${TOOL_START}]:${RESET} ${call.name} => ${Engine.tools[call.name]?.summarizeInput(call.args)}\n`,
                );
              }

              toolCalls[call.id!] = { call, result: undefined };
              if (this.currentTurnState) {
                this.currentTurnState.toolCalls.push(call);
                this.currentTurnState.pendingToolIds.add(call.id!);
              }

              const startTime = performance.now();
              const promise = this.executeTool(call.name, call.args, {
                toolCallId: call.id!,
                turnIndex: this.turnCounter,
                threadId: this.context.identity.threadId,
              })
                .then((result) => {
                  const durationMs = performance.now() - startTime;
                  const toolResultWrapper = result;

                  const summarizedResult = this.computeSummarizedResult(tool, toolResultWrapper, call.args);

                  Engine.emitToolCallEnd(this.context.identity.threadId, {
                    agentId: this.id,
                    readableName: this.readableName,
                    toolName: call.name,
                    callId: call.id!,
                    result: toolResultWrapper,
                    summary: summarizedResult,
                    durationMs,
                    success: toolResultWrapper.success,
                    turnCount: this.turnCounter,
                  });
                  
                  toolCalls[call.id!]!.result = toolResultWrapper;

                  if (Engine.prettyPrint) {
                    process.stdout.write(
                      `\n${TOOL_START}[${this.readableName}/T${this.turnCounter} ${THINKING}RESULT${RESET}${TOOL_START}]:${RESET} ${call.name} => ${summarizedResult}\n`,
                    );
                  }

                  if (this.currentTurnState) {
                    this.currentTurnState.pendingToolIds.delete(call.id!);
                    this.currentTurnState.toolResults.push({ id: call.id!, result: toolResultWrapper });
                  }
                })
                .catch((e) => {
                  const durationMs = performance.now() - startTime;
                  const errorWrapper: ToolResult = {
                    success: false,
                    summary: "Tool execution failed",
                    error: e instanceof Error ? e.message : String(e),
                  };
                  Engine.emitToolCallEnd(this.context.identity.threadId, {
                    agentId: this.id,
                    readableName: this.readableName,
                    toolName: call.name,
                    callId: call.id!,
                    result: errorWrapper,
                    summary: errorWrapper.error!,
                    durationMs,
                    success: false,
                    turnCount: this.turnCounter,
                  });
                  toolCalls[call.id!]!.result = errorWrapper;

                  if (Engine.prettyPrint) {
                    process.stdout.write(
                      `\n${TOOL_START}[${this.readableName}/T${this.turnCounter} ${ERROR}ERROR${RESET}${TOOL_START}]:${RESET} ${call.name} => ${errorWrapper.error}\n`,
                    );
                  }
                });
              pendingPromises.push(promise);
              break;
            }
            case "usage":
              if (!event.usage) break;
              this.budgetTracker.recordUsage(event.usage.promptTokens, event.usage.completionTokens);
              this.context.state.metrics.totalTokens = event.usage.totalTokens;
              this.context.state.metrics.lastTurnTokens = event.usage.totalTokens - this.context.state.metrics.totalTokens;
              this.context.state.metrics.lastPromptTokens = event.usage.promptTokens;

              Engine.emitAgentMetrics(this.context.identity.threadId, {
                agentId: this.id,
                tokensUsed: event.usage.totalTokens,
                tokensLimit: this.getModelInfo()?.ctx ?? DEFAULT_MODEL_CTX,
                contextUsage: this.getContextUsagePercent(),
              });
              break;
            case "request_sent":
              Engine.emitProviderRequest(this.context.identity.threadId, {
                agentId: this.id,
                readableName: this.readableName,
                request: event.request ?? {},
                turnCount: this.turnCounter,
              });
              break;
          }
        }
      } catch (e: any) {
        if (this.abortController.signal.aborted) {
          return { type: AgentActType.Response, response: { isUser: false, content: "**[Interrupted]**", timestamp: Date.now(), tokens: 0 } };
        }
        throw e;
      }

      await Promise.all(pendingPromises);

      // Detect empty response
      const isEmptyResponse =
        accumulatedContent.trim() === "" &&
        accumulatedReasoning.trim() === "" &&
        Object.keys(toolCalls).length == 0;
      
      if (isEmptyResponse) {
        logger.warn(`[${this.readableName}] Empty response from provider.`);
        return {
          type: AgentActType.Response,
          response: {
            isUser: false,
            content: "[EMPTY_RESPONSE]",
            timestamp: Date.now(),
            tokens: 0,
          },
        };
      }

      this.rollingBuffer.startNewTurn();
      for (const tc of Object.values(toolCalls)) {
        if (tc.result) {
          const res = tc.result;
          const content = typeof res.output === 'string' ? res.output : JSON.stringify(res.output ?? res);
          const priority: EntryPriority = res.success ? "normal" : "critical";
          this.rollingBuffer.push(tc.call.id!, tc.call.name, content, priority);
        }
      }

      const turn: AgentTurn = {
        timestamp: Date.now(),
        reasoning: accumulatedReasoning,
        content: accumulatedContent,
        toolCalls: Object.values(toolCalls).map(tc => tc.call),
        toolResults: Object.entries(toolCalls).map(([id, tc]) => ({ id, result: tc.result })),
        tokens: this.context.state.metrics.lastTurnTokens,
      };

      if (this.context.io.onTurn) {
        // Log to verify wiring
        logger.debug(`[${this.readableName}] Calling onTurn to persist history...`);
        await this.context.io.onTurn(turn, this.id);
      } else {
        logger.warn(`[${this.readableName}] onTurn callback is MISSING! History will not be persisted.`);
      }
        history.workflow.turns.push(turn);
      } else if (history.type === AgentWorkType.Conversational) {
        if (!history.conversation) {
          history.conversation = { history: [] };
        }
        // Conversational turns with tool calls are treated as internal workflow steps
        history.conversation.history.push({
          turns: [turn],
          timestamp: turn.timestamp,
          completed: true,
          finalMessage: typeof turn.content === 'string' ? turn.content : (Array.isArray(turn.content) ? turn.content.map((p: any) => p.text || '').join('') : null)
        });
      }

      if (this.context.io.onTurn) {
        // We still call the outer callback for persistence
        await this.context.io.onTurn(turn, this.id);
      } else {
        logger.warn(`[${this.readableName}] onTurn callback is MISSING! History will not be persisted.`);
      }

      // Emit turn complete event for event persistence cleanup
      Engine.emitAgentTurnComplete(this.context.identity.threadId, {
        agentId: this.id,
        readableName: this.readableName,
        turnCount: this.turnCounter,
      });

      // Auto-checkpoint after turn
      if (this.context.io.onCheckpoint) {
        await this.context.io.onCheckpoint(this.id);
      }

      const termination = this.detectTermination(accumulatedContent);
      if (termination.terminated) {
        this.setStatus("idle");
        Engine.emitAgentTerminated(this.context.identity.threadId, {
          agentId: this.id,
          readableName: this.readableName,
          reason: termination.visibleContent || "done",
          success: true,
        });

        return {
          type: AgentActType.Response,
          response: {
            isUser: false,
            content: termination.visibleContent,
            timestamp: Date.now(),
            tokens: this.context.state.metrics.lastTurnTokens,
            reasoning: accumulatedReasoning,
          },
        };
      }

      this.setStatus("idle");
      return { type: AgentActType.Turn, turn };

    } finally {
      this.currentTurnState = undefined;
      if (this.status === "working" && !this.isExecutingLoop) {
        this.setStatus("idle");
      }
    }
  }

  private computeSummarizedResult(tool: any, output: any, inputArgs: any): string {
    if (output && typeof output === "object" && output.summary) return output.summary;
    if (typeof tool.summary === "function") return tool.summary(output, inputArgs);
    return "Operation complete";
  }

  private getProviderTools(): ProviderTool[] {
    return Object.values(Engine.tools)
      .filter((t) => this.context.environment.permissions.scopes.includes(t.metadata.scope))
      .map((t) => toolToProviderTool(t));
  }

  private detectTermination(content: string): { terminated: boolean; visibleContent: string } {
    const index = content.indexOf(DONE_MARKER);
    if (index !== -1) return { terminated: true, visibleContent: content.substring(0, index).trim() };
    return { terminated: false, visibleContent: content };
  }

  private getContextUsagePercent() { 
    return (this.context.state.metrics.totalTokens / (this.getModelInfo()?.ctx || DEFAULT_MODEL_CTX)) * 100; 
  }

  async actUntilAgentEnds(): Promise<AgentActResult[]> {
    this.isExecutingLoop = true;
    const accumulatedActions: AgentActResult[] = [];
    let consecutiveEmptyCount = 0;
    const MAX_EMPTY_RETRIES = 3;

    try {
      while (true) {
        const res = await this.act();
        accumulatedActions.push(res);

        if (res.type === AgentActType.Response) {
          if (res.response?.content === "[EMPTY_RESPONSE]") {
            consecutiveEmptyCount++;
            logger.warn(`[${this.readableName}] Empty response ${consecutiveEmptyCount}/${MAX_EMPTY_RETRIES}`);
            if (consecutiveEmptyCount >= MAX_EMPTY_RETRIES) {
              logger.error(`[${this.readableName}] Too many empty responses. Terminating.`);
              break;
            }
            continue;
          }
          break;
        }
        consecutiveEmptyCount = 0;
      }
    } finally {
      this.isExecutingLoop = false;
      this.setStatus("completed" as any);
    }
    return accumulatedActions;
  }

  async executeTool(name: string, args: any, toolCtx: any): Promise<ToolResult> {
    const tool = Engine.tools[name];
    if (!tool) throw new Error(`Tool not found: ${name}`);
    
    const thread = Engine.getThread(this.context.identity.threadId);
    return await tool.execute(args, { 
      ...toolCtx, 
      agent: this, 
      host: this.context.environment.host, 
      coordinator: thread!.coordinator as ICoordinator 
    });
  }

  async interrupt(): Promise<void> {
    if (!this.abortController) return;
    this.abortController.abort();
    this.status = "completed";

    if (this.currentTurnState) {
      const partialTurn: AgentTurn = {
        toolCalls: this.currentTurnState.toolCalls,
        toolResults: this.currentTurnState.toolResults.map((tr) => {
          const result = tr.result as { status?: string } | undefined;
          return {
            ...tr,
            result: result?.status === "pending"
              ? { status: "killed", error: "Interrupted by user" }
              : tr.result,
          };
        }),
        reasoning: this.currentTurnState.reasoning,
        content: this.currentTurnState.content,
        timestamp: Date.now(),
        tokens: this.context.state.metrics.lastTurnTokens,
        interrupted: true,
        completed: false,
      };

      for (const toolId of this.currentTurnState.pendingToolIds) {
        const existing = partialTurn.toolResults.find((tr) => tr.id === toolId);
        if (!existing) {
          partialTurn.toolResults.push({
            id: toolId,
            result: { status: "killed", error: "Interrupted by user" },
          });
        }
      }

      if (this.context.io.onTurn) {
        await this.context.io.onTurn(partialTurn, this.id);
      }

      this.currentTurnState = undefined;
    }

    this.setStatus("completed");
  }

  public async forgetLastTurn(): Promise<void> {
    const lastTurn = this.turnCounter;
    if (lastTurn <= 0) return;
    this.turnCounter--;
    this.setStatus("completed");
  }

  public updateGenerationOptions(options: Partial<GenerationOptions>): void {
    this.context.execution.generationOptions = {
      ...this.context.execution.generationOptions,
      ...options,
    } as any;

    if (this.context.io.onCheckpoint) {
      this.context.io.onCheckpoint(this.id);
    }

    Engine.emitAgentModelChanged(this.context.identity.threadId, {
      agentId: this.id,
      readableName: this.readableName,
      newModelId: options.modelId as string,
      newProviderId: options.providerId as string,
    });
  }

  private checkContextLimits() {
    const usagePercent = this.getContextUsagePercent();
    if (usagePercent >= CONTEXT_CRITICAL_THRESHOLD * 100) {
      logger.warn(`[${this.readableName}] Context at critical level: ${usagePercent.toFixed(1)}%`);
    }
  }

  private async checkCompaction() {
    if (this.context.execution.disableCompaction || this.isCompacting) return;
    this.isCompacting = true;
    try {
      const usagePercent = this.getContextUsagePercent();
      if (usagePercent > CONTEXT_COMPACTION_THRESHOLD * 100) {
        logger.info(`[${this.readableName}] Context usage at ${usagePercent.toFixed(1)}%. Budget eviction required.`);
        await this.enforceBudget();
      }
      const completedCheckpoints = this.parallelCompactor.getCompletedCheckpoints();
      if (completedCheckpoints.length > this.checkpoints.length) {
        this.checkpoints = completedCheckpoints;
        this.context.state.checkpoints = this.checkpoints;
      }
    } catch (e: any) {
      logger.error(`[${this.readableName}] Compaction failed: ${e.message}`);
    } finally {
      this.isCompacting = false;
    }
  }

  private async enforceBudget(): Promise<void> {
    const alloc = this.budgetTracker.getAllocation();
    const currentRollingUsage = this.rollingBuffer.getTokenCount();

    if (currentRollingUsage > alloc.rollingBudget * 0.9) {
      const targetTokens = Math.floor(alloc.rollingBudget * 0.7);
      const evicted = this.rollingBuffer.evictToBudget(targetTokens);

      if (evicted.length > 0) {
        logger.info(`[${this.readableName}] Evicted ${evicted.length} entries from rolling buffer.`);
        // @ts-ignore
        this.parallelCompactor.queueCompaction(
          evicted as BufferEntry[],
          this.context.identity.threadId,
          this.id,
        );
      }
    }

    const turnEvicted = this.rollingBuffer.trimToTurnLimit();
    if (turnEvicted.length > 0) {
      // @ts-ignore
      this.parallelCompactor.queueCompaction(
        turnEvicted as BufferEntry[],
        this.context.identity.threadId,
        this.id,
      );
    }
  }

  static createHistory(
    type: AgentWorkType,
    initialMessage: ACM,
  ): AgentHistory {
    switch (type) {
      case AgentWorkType.Conversational:
        return {
          type: AgentWorkType.Conversational,
          conversation: { history: [initialMessage] },
        };
      case AgentWorkType.Goal:
        return {
          type: AgentWorkType.Goal,
          workflow: {
            turns: [],
            timestamp: Date.now(),
            completed: false,
            finalMessage: null,
          },
        };
    }
  }

  getBudgetState() {
    return {
      budget: this.budgetTracker.getState(),
      checkpoints: this.checkpoints,
      protectedContext: this.protectedContext.getState(),
      fileWatchBudget: this.fileWatchBudget.getState(),
    };
  }
}
