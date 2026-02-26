import { ZaiProvider, LMStudioProvider, ZenProvider, NanoGPTProvider } from "./providers/index";
import type { IHost } from "@firmius/shared";
import type { IThread } from "@firmius/shared";
import type {
  AgentContentEvent,
  AgentFileChangedEvent,
  AgentModelChangedEvent,
  AgentMetricsEvent,
  AgentSpawnedEvent,
  AgentTerminatedEvent,
  AgentThinkingEvent,
  EngineEvent,
  IEngine,
  IgniteOptions,
  ToolCallEndEvent,
  ToolCallStartEvent,
  ToolCallPreparingEvent,
  ToolCallUpdateEvent,
  UserMessageEvent,
  AgentStatusEvent,
  ProcessOutputEvent,
  ProcessExitEvent,
  AgentProviderRequestEvent,
  AgentProviderErrorEvent,
  AgentTurnCompleteEvent,
} from "@firmius/shared";
import type { IProvider } from "@firmius/shared";
import type { ITool } from "@firmius/shared";
import { EventEmitter } from "node:events";
import { AllFileTools, AllProcessTools, AllDelegationTools, AllContextTools, AllCompactionTools, AllWebTools, AllTodoTools, AllCodeTools, AllGitTools, ReportProgressTool, AllSubagentTools } from "./tools/index";
import { DEFAULT_PROVIDER } from "./Constants";
import { ProcessManager } from "./ProcessManager";
import { AgentFactory } from "./AgentFactory";
import { LSPUtility } from "./lsp/index";
import { purposeRegistry } from "./registry/PurposeRegistry";

export interface EngineEventEmitter extends EventEmitter {
  on(event: 'agent_spawned', listener: (event: AgentSpawnedEvent) => void): this;
  on(event: 'agent_thinking', listener: (event: AgentThinkingEvent) => void): this;
  on(event: 'agent_content', listener: (event: AgentContentEvent) => void): this;
  on(event: 'agent_provider_request', listener: (event: AgentProviderRequestEvent) => void): this;
  on(event: 'agent_model_changed', listener: (event: AgentModelChangedEvent) => void): this;
  on(event: 'tool_call_start', listener: (event: ToolCallStartEvent) => void): this;
  on(event: 'tool_call_end', listener: (event: ToolCallEndEvent) => void): this;
  on(event: 'agent_terminated', listener: (event: AgentTerminatedEvent) => void): this;
  on(event: 'agent_file_changed', listener: (event: AgentFileChangedEvent) => void): this;
  on(event: 'user_message', listener: (event: UserMessageEvent) => void): this;
  on(event: 'agent_status', listener: (event: AgentStatusEvent) => void): this;
  on(event: 'agent_metrics', listener: (event: AgentMetricsEvent) => void): this;
  on(event: 'process_output', listener: (event: ProcessOutputEvent) => void): this;
  on(event: 'process_exit', listener: (event: ProcessExitEvent) => void): this;
  on(event: 'agent_provider_error', listener: (event: AgentProviderErrorEvent) => void): this;
  on(event: 'tool_call_preparing', listener: (event: ToolCallPreparingEvent) => void): this;
  on(event: 'agent_turn_complete', listener: (event: AgentTurnCompleteEvent) => void): this;
  on(event: 'tool_call_update', listener: (event: ToolCallUpdateEvent) => void): this;
  emit(event: 'agent_spawned', payload: AgentSpawnedEvent): boolean;
  emit(event: 'agent_thinking', payload: AgentThinkingEvent): boolean;
  emit(event: 'agent_content', payload: AgentContentEvent): boolean;
  emit(event: 'agent_provider_request', payload: AgentProviderRequestEvent): boolean;
  emit(event: 'agent_model_changed', payload: AgentModelChangedEvent): boolean;
  emit(event: 'tool_call_start', payload: ToolCallStartEvent): boolean;
  emit(event: 'tool_call_end', payload: ToolCallEndEvent): boolean;
  emit(event: 'agent_terminated', payload: AgentTerminatedEvent): boolean;
  emit(event: 'agent_file_changed', payload: AgentFileChangedEvent): boolean;
  emit(event: 'user_message', payload: UserMessageEvent): boolean;
  emit(event: 'agent_status', payload: AgentStatusEvent): boolean;
  emit(event: 'agent_metrics', payload: AgentMetricsEvent): boolean;
  emit(event: 'process_output', payload: ProcessOutputEvent): boolean;
  emit(event: 'process_exit', payload: ProcessExitEvent): boolean;
  emit(event: 'agent_provider_error', payload: AgentProviderErrorEvent): boolean;
  emit(event: 'tool_call_preparing', payload: ToolCallPreparingEvent): boolean;
  emit(event: 'agent_turn_complete', payload: AgentTurnCompleteEvent): boolean;
  emit(event: 'tool_call_update', payload: ToolCallUpdateEvent): boolean;
}

class EngineInstance implements IEngine {
  providers: Record<string, IProvider> = {};
  tools: Record<string, ITool<any, any>> = {};
  processManager: ProcessManager = new ProcessManager();
  agentFactory: AgentFactory = new AgentFactory();
  lspUtilities: Map<IHost, Map<string, LSPUtility>> = new Map();
  eventEmitter: EngineEventEmitter;
  eventHistory: EngineEvent[] = [];
  prettyPrint: boolean = false;
  threads: Map<string, IThread> = new Map();
  private readonly MAX_EVENT_HISTORY = 1000;
  private toolArgsPerAgent: Map<string, Record<string, unknown>> = new Map();

  constructor() {
    this.eventEmitter = new EventEmitter() as EngineEventEmitter;
  }

  async ignite(options?: IgniteOptions) {
    this.loadTools();
    this.loadProviders();
    await purposeRegistry.init();
    if (options?.prettyPrint !== undefined) {
      this.prettyPrint = options.prettyPrint;
    }

    // Hook up ProcessManager output events
    this.processManager.on(
      "output",
      (data: { id: string; data: string; src: "stdout" | "stderr" }) => {
        // We don't have threadId here, so use a default or track process->threadId mapping
        // For now, we'll emit without threadId and let the frontend handle it
        this.emitProcessOutput("", {
          processId: data.id,
          data: data.data,
          source: data.src,
        });
      },
    );

    this.processManager.on(
      "exit",
      (data: {
        id: string;
        pid: number;
        exitCode: number;
        durationMs: number;
      }) => {
        this.emitProcessExit("", {
          processId: data.id,
          pid: data.pid,
          exitCode: data.exitCode,
          durationMs: data.durationMs,
        });
      },
    );
  }

  getThread(id: string): IThread | undefined {
    return this.threads.get(id);
  }

  addThread(thread: IThread): void {
    if (this.threads.has(thread.id)) {
      return;
    }
    this.threads.set(thread.id, thread);
  }

  removeThread(id: string): void {
    this.threads.delete(id);
  }

  loadProviders() {
    const zaiKey = process.env.ZAI_API_KEY;
    if (!zaiKey) {
      throw new Error("ZAI_API_KEY environment variable is required");
    }
    this.providers[DEFAULT_PROVIDER] = new ZaiProvider(zaiKey);
    const lmstudioKey = process.env.LMSTUDIO_API_KEY || "lm-studio";
    this.providers.lmstudio = new LMStudioProvider(lmstudioKey);
    this.providers.opencode = new ZenProvider("");
    this.providers.nanogpt = new NanoGPTProvider();
  }

  loadTools() {
    AllFileTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllProcessTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllDelegationTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllContextTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllCompactionTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllWebTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllTodoTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllCodeTools.forEach((t) => (this.tools[t.metadata.name] = t));
    AllGitTools.forEach((t) => (this.tools[t.metadata.name] = t));
    this.tools[ReportProgressTool.metadata.name] = ReportProgressTool;

    AllSubagentTools.forEach((t) => (this.tools[t.metadata.name] = t));
  }

  getLSPUtility(host: IHost, rootUri: string): LSPUtility {
    if (!this.lspUtilities.has(host)) {
      this.lspUtilities.set(host, new Map());
    }
    const hostMap = this.lspUtilities.get(host)!;
    if (!hostMap.has(rootUri)) {
      hostMap.set(rootUri, new LSPUtility(host, rootUri));
    }
    return hostMap.get(rootUri)!;
  }

  getEventHistory(): EngineEvent[] {
    return [...this.eventHistory];
  }

  private addEventToHistory(event: EngineEvent): void {
    this.eventHistory.push(event);
    if (this.eventHistory.length > this.MAX_EVENT_HISTORY) {
      this.eventHistory.shift();
    }
  }

  emitAgentSpawned(
    threadId: string,
    event: Omit<AgentSpawnedEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentSpawnedEvent = {
      ...event,
      type: "agent_spawned",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_spawned", fullEvent);
  }

  emitAgentThinking(
    threadId: string,
    event: Omit<AgentThinkingEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentThinkingEvent = {
      ...event,
      type: "agent_thinking",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_thinking", fullEvent);
  }

  emitAgentContent(
    threadId: string,
    event: Omit<AgentContentEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentContentEvent = {
      ...event,
      type: "agent_content",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_content", fullEvent);
  }

  emitProviderRequest(
    threadId: string,
    event: Omit<AgentProviderRequestEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentProviderRequestEvent = {
      ...event,
      type: "agent_provider_request",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_provider_request", fullEvent);
  }

  emitToolCallStart(
    threadId: string,
    event: Omit<ToolCallStartEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const tool = this.tools[event.toolName];
    const summary = tool
      ? tool.summarizeInput(event.arguments || {})
      : event.toolName;
    const fullEvent: ToolCallStartEvent = {
      ...event,
      type: "tool_call_start",
      threadId,
      timestamp: Date.now(),
      summary,
      arguments: event.arguments,
      agentId: event.agentId,
      readableName: event.readableName,
      toolName: event.toolName,
      callId: event.callId,
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("tool_call_start", fullEvent);
    this.toolArgsPerAgent.set(event.callId, event.arguments || {});
  }

  emitToolCallEnd(
    threadId: string,
    event: Omit<ToolCallEndEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const tool = this.tools[event.toolName];
    let summary = event.summary || event.toolName;

    if (tool && !event.summary) {
      const args = this.toolArgsPerAgent.get(event.callId) || {};
      // result is now expected to be a ToolResult or at least compatible
      const resultObj = typeof event.result === 'string' ? { success: event.success, summary: event.result } : (event.result as any);
      if (tool.summary) {
        summary = tool.summary(resultObj, args);
      } else {
        summary = resultObj.summary || summary;
      }
    }

    const fullEvent: ToolCallEndEvent = {
      ...event,
      type: "tool_call_end",
      threadId,
      timestamp: Date.now(),
      summary,
      result: event.result,
      agentId: event.agentId,
      readableName: event.readableName,
      toolName: event.toolName,
      callId: event.callId,
      durationMs: event.durationMs,
      success: event.success,
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("tool_call_end", fullEvent);
    this.toolArgsPerAgent.delete(event.callId);
  }

  emitToolCallUpdate(
    threadId: string,
    event: Omit<ToolCallUpdateEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: ToolCallUpdateEvent = {
      ...event,
      type: "tool_call_update",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("tool_call_update", fullEvent);
  }

  emitAgentTerminated(
    threadId: string,
    event: Omit<AgentTerminatedEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentTerminatedEvent = {
      ...event,
      type: "agent_terminated",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_terminated", fullEvent);
  }

  emitAgentFileChanged(
    threadId: string,
    event: Omit<AgentFileChangedEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentFileChangedEvent = {
      ...event,
      type: "agent_file_changed",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_file_changed", fullEvent);
  }

  emitAgentModelChanged(
    threadId: string,
    event: Omit<AgentModelChangedEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentModelChangedEvent = {
      ...event,
      type: "agent_model_changed",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_model_changed", fullEvent);
  }

  emitProcessOutput(
    threadId: string,
    event: Omit<ProcessOutputEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: ProcessOutputEvent = {
      ...event,
      type: "process_output",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("process_output", fullEvent);
  }

  emitProcessExit(
    threadId: string,
    event: Omit<ProcessExitEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: ProcessExitEvent = {
      ...event,
      type: "process_exit",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("process_exit", fullEvent);
  }

  emitUserMessage(
    threadId: string,
    event: Omit<UserMessageEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: UserMessageEvent = {
      ...event,
      type: "user_message",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("user_message", fullEvent);
  }

  emitAgentStatus(
    threadId: string,
    event: Omit<AgentStatusEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentStatusEvent = {
      ...event,
      type: "agent_status",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_status", fullEvent);
  }

  emitAgentMetrics(
    threadId: string,
    metrics: {
      agentId: string;
      tokensUsed: number;
      tokensLimit: number;
      contextUsage: number;
    },
  ): void {
    const fullEvent: AgentMetricsEvent = {
      type: "agent_metrics",
      timestamp: Date.now(),
      threadId,
      agentId: metrics.agentId,
      tokensUsed: metrics.tokensUsed,
      tokensLimit: metrics.tokensLimit,
      contextUsage: metrics.contextUsage,
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_metrics", fullEvent);
  }

  emitAgentProviderError(
    threadId: string,
    event: Omit<AgentProviderErrorEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentProviderErrorEvent = {
      ...event,
      type: "agent_provider_error",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_provider_error", fullEvent);
  }

  emitAgentTurnComplete(
    threadId: string,
    event: Omit<AgentTurnCompleteEvent, "timestamp" | "type" | "threadId">,
  ): void {
    const fullEvent: AgentTurnCompleteEvent = {
      ...event,
      type: "agent_turn_complete",
      threadId,
      timestamp: Date.now(),
    };
    this.addEventToHistory(fullEvent);
    this.eventEmitter.emit("agent_turn_complete", fullEvent);
  }
}

export const Engine = new EngineInstance();
