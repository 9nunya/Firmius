import { create } from "zustand";
import { client } from "@firmius/shared/api";
import { parseSSEMessage, type ParsedEvent } from "@firmius/shared/sse";
import type {
  Thread,
  Message,
  Agent,
  APIError as BackendAPIError,
  ThreadGenerationOptions,
  CreateThreadRequest,
  ProviderInfo,
  UserConfig,
} from "@firmius/shared/api";
import type { Event } from "@/types";
import type { AgentStatusChangedEvent, ThreadCreatedEvent, AgentSpawnedEvent, ToolCallStartEvent, ToolCallEndEvent, ToolCallUpdateEvent, AgentThinkingEvent, AgentContentEvent, AgentProviderRequestEvent, AgentTerminatedEvent, AgentMetricsEvent, AgentProviderErrorEvent, MessageAddedEvent, ProcessOutputEvent, ProcessExitEvent } from "@/types";

export interface LiveActivity {
  toolName: string;
  description: string;
  status: string;
  startedAt: Date;
}

let TEMP_SEQ_COUNTER = 0;
const generateTempSequence = (): number => {
  TEMP_SEQ_COUNTER++;
  return -TEMP_SEQ_COUNTER;
};

const MAX_EVENTS = 1000;

// Build version check for iOS PWA cache busting
const BUILD_TIMESTAMP = process.env.NEXT_PUBLIC_BUILD_TIMESTAMP;
const STORED_VERSION = typeof window !== 'undefined' ? localStorage.getItem('app-version') : null;

// Clear cache if build version mismatch (iOS PWA fix)
if (typeof window !== 'undefined' && BUILD_TIMESTAMP && STORED_VERSION !== BUILD_TIMESTAMP) {
  console.log('[App] Version mismatch, clearing cached data...');
  localStorage.removeItem('app-store');
  localStorage.setItem('app-version', BUILD_TIMESTAMP);
}

function addEvent(events: Event[], newEvent: Event): Event[] {
  const updated = [...events, newEvent];
  if (updated.length > MAX_EVENTS) {
    return updated.slice(-MAX_EVENTS);
  }
  return updated;
}

function normalizeThreadData(thread: any): Thread {
  return {
    ...thread,
    checkpointedAt: new Date(thread.checkpointedAt),
  };
}

function normalizeMessageData(message: any): Message {
  return {
    ...message,
    timestamp: message.timestamp ? new Date(message.timestamp) : new Date(),
  };
}

function getEventTimestamp(event: { timestamp: Date | string | number }): number {
  if (event.timestamp instanceof Date) return event.timestamp.getTime();
  return new Date(event.timestamp).getTime();
}

type ConnectionStatus = "connected" | "disconnected" | "connecting";

interface AppState {
  threads: Thread[];
  activeThreadId: string | null;
  messages: Message[];
  agents: Agent[];
  activeAgentId: string | null;
  connectionStatus: ConnectionStatus;
  error: BackendAPIError | null;
  events: Event[];
  selectedAgentId: string | null;
  providers: ProviderInfo[];
  _isReplay: boolean;
  isFleetSidebarOpen: boolean;
  sidebarTab: "threads" | "settings";
  rightSidebarTab: "fleet" | "todos" | "changes";
  isRightSidebarOpen: boolean;
  userConfig: UserConfig | null;
  providerRequestData: { messages: unknown[]; modelId: string } | null;
  showProviderRequestModal: boolean;
}

interface AppActions {
  selectThread: (threadId: string) => Promise<void>;
  createThread: (data: CreateThreadRequest) => Promise<void>;
  sendMessage: (content: string | any[]) => Promise<void>;
  interruptThread: () => Promise<void>;
  editMessage: (sequence: number, content: string) => Promise<void>;
  branchThread: (sequence: number, content: string) => Promise<void>;
  deleteMessage: (sequence: number) => Promise<void>;
  undoToMessage: (sequence: number) => Promise<void>;
  undoLastTurn: (agentId: string) => Promise<void>;
  restoreMessage: (sequence: number) => Promise<void>;
  focusAgent: (agentId: string | null) => void;
  setAgents: (agents: Agent[]) => void;
  updateAgentModel: (agentId: string, modelId: string) => void;
  loadThreads: () => Promise<void>;
  deleteThread: (threadId: string) => Promise<void>;
  loadProviders: () => Promise<void>;
  updateThreadSettings: (settings: ThreadGenerationOptions) => Promise<void>;
  updateFromEvent: (event: Event) => void;
  toggleFleetSidebar: () => void;
  setSidebarTab: (tab: "threads" | "settings") => void;
  setRightSidebarTab: (tab: "fleet" | "todos" | "changes") => void;
  toggleRightSidebar: () => void;
  openRightSidebar: () => void;
  closeRightSidebar: () => void;
  fetchUserConfig: () => Promise<void>;
  updateUserConfig: (config: UserConfig) => Promise<void>;
}

type AppStore = AppState & AppActions;

const useAppStore = create<AppStore>((set, get) => ({
  threads: [],
  activeThreadId: null,
  error: null,
  events: [],
  messages: [],
  agents: [],
  activeAgentId: null,
  selectedAgentId: null,
  providers: [],
  connectionStatus: "disconnected",
  _isReplay: false,
  isFleetSidebarOpen: true,
  sidebarTab: "threads",
  rightSidebarTab: "fleet",
  isRightSidebarOpen: false,
  userConfig: null,
  providerRequestData: null,
  showProviderRequestModal: false,

  interruptThread: async () => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;
    try {
      await client.interruptThread(activeThreadId);
    } catch (error) {
      console.error("Failed to interrupt thread:", error);
    }
  },

  loadProviders: async () => {
    try {
      const providers = await client.getProviders();
      set({ providers });
    } catch (error) {
      console.error("Failed to load providers:", error);
    }
  },

  updateThreadSettings: async (settings: ThreadGenerationOptions) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;
    try {
      await client.updateThreadSettings(activeThreadId, settings);
      const updatedThread = await client.getThread(activeThreadId);
      set((state) => ({
        threads: state.threads.map((t) =>
          t.id === activeThreadId ? normalizeThreadData(updatedThread) : t
        ),
        error: null,
      }));
    } catch (error: any) {
      console.error("Failed to update thread settings:", error);
      set({ error: error.error ? error : { error: "Failed to update settings" } as any });
    }
  },

  branchThread: async (sequence: number, content: string) => {
    const { activeThreadId, selectThread } = get();
    if (!activeThreadId) return;
    try {
      await client.branchThread(activeThreadId, sequence, content);
      await selectThread(activeThreadId);
    } catch (error) {
      console.error("Failed to branch thread:", error);
    }
  },

  undoToMessage: async (sequence: number) => {
    const { activeThreadId, selectThread } = get();
    if (!activeThreadId) return;
    try {
      const response = await fetch(`/api/threads/${activeThreadId}/messages/${sequence}/undo`, {
        method: "POST",
      });
      if (!response.ok) {
        throw new Error(`Undo failed: ${response.statusText}`);
      }
      await selectThread(activeThreadId);
    } catch (error) {
      console.error("Failed to undo to message:", error);
      set({ error: { error: "Failed to undo turn" } as any });
    }
  },

  undoLastTurn: async (agentId: string) => {
    const { activeThreadId, selectThread } = get();
    if (!activeThreadId) return;
    try {
      const response = await fetch(`/api/threads/${activeThreadId}/agents/${agentId}/undo-turn`, {
        method: "POST",
      });
      if (!response.ok) {
        throw new Error(`Undo turn failed: ${response.statusText}`);
      }
      await selectThread(activeThreadId);
    } catch (error) {
      console.error("Failed to undo last turn:", error);
      set({ error: { error: "Failed to undo agent turn" } as any });
    }
  },

  createThread: async (data: CreateThreadRequest) => {
    try {
      const thread = await client.createThread(data);
      const normalizedThread = normalizeThreadData(thread);
      set((state) => ({
        threads: [normalizedThread, ...state.threads],
        activeThreadId: normalizedThread.id,
      }));
      await get().selectThread(normalizedThread.id);
    } catch (error) {
      console.error("Failed to create thread:", error);
      throw error;
    }
  },

  selectThread: async (threadId: string) => {
    set({ activeThreadId: threadId });

    const url = new URL(window.location.href);
    url.searchParams.set("thread", threadId);
    window.history.replaceState({}, "", url.toString());

    try {
      const [messages, eventsData] = await Promise.all([
        client.getMessages(threadId),
        client.getThreadEvents(threadId),
      ]);

      let finalAgents = eventsData.agents || [];
      if (finalAgents.length === 0) {
        finalAgents = await client.getAgents(threadId);
      }

      if (finalAgents.length === 0) {
        const currentThread = get().threads.find(t => t.id === threadId);
        if (currentThread?.leadAgentId) {
          finalAgents = [{
            id: currentThread.leadAgentId,
            readableName: "Lead Agent",
            purpose: "Main Assistant",
            isLead: true,
            status: "idle",
            modelId: "default",
            threadId: threadId,
            turnCount: 1,
            objective: "",
            subagentIds: []
          }];
        }
      }

      const rawEvents = eventsData.events ?? [];
      const parsedEvents = rawEvents
        .map((e: any) => parseSSEMessage(e))
        .filter((e: ParsedEvent | null): e is ParsedEvent => e !== null) as unknown as Event[];

      const sortedEvents = [...parsedEvents].sort((a, b) =>
        getEventTimestamp(a) - getEventTimestamp(b)
      );

      const seen = new Set<string>();
      const uniqueEvents = sortedEvents.filter((event) => {
        const agentId = 'agentId' in event ? (event as any).agentId : '';
        let contentHash = '';
        if (event.type === 'agent_thinking' || event.type === 'agent_content') {
          const data = (event as any).thought || (event as any).content || '';
          contentHash = String(data).substring(0, 50);
        }
        const key = `${event.type}:${agentId}:${getEventTimestamp(event)}:${contentHash}`;
        if (seen.has(key)) return false;
        seen.add(key);
        return true;
      });

      set({
        messages: (messages || []).map(normalizeMessageData),
        agents: finalAgents,
        activeAgentId: null,
        _isReplay: true,
      });

      const { updateFromEvent } = get();
      for (const event of uniqueEvents) {
        updateFromEvent(event);
      }

      set({ _isReplay: false });

      const finalAgentsWithStatus = finalAgents.map(agent => {
        const statusEvents = uniqueEvents
          .filter(e => e.type === 'agent_status_changed' && e.agentId === agent.id)
          .sort((a, b) => getEventTimestamp(b) - getEventTimestamp(a));

        if (statusEvents.length > 0) {
          return { ...agent, status: (statusEvents[0] as AgentStatusChangedEvent).status };
        }
        return agent;
      });

      set({
        agents: finalAgentsWithStatus,
        events: uniqueEvents,
      });

    } catch (error) {
      console.error("Failed to load thread:", error);
    }
  },

  sendMessage: async (content: string | any[]) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;

    const state = get();
    const messages = state.messages;
    const lastUserMsgIndex = messages.map(m => m.isUser).lastIndexOf(true);

    const keptMessages = lastUserMsgIndex >= 0
      ? messages.slice(lastUserMsgIndex)
      : messages;

    const tempMessage: Message = {
      sequence: generateTempSequence(),
      isUser: true,
      content,
      timestamp: new Date(),
      tokens: 0,
      type: "response",
    };

    set((state: AppStore) => ({
      messages: [...keptMessages, tempMessage],
    }));

    try {
      const confirmedMessage = await client.sendMessage(activeThreadId, content);
      set((state: AppStore) => {
        const alreadyExists = state.messages.some(m => m.sequence === confirmedMessage.sequence);
        if (alreadyExists) {
          return {
            messages: state.messages.filter(m => m.sequence !== tempMessage.sequence)
          };
        }
        return {
          messages: state.messages.map((m: Message) =>
            m.sequence === tempMessage.sequence
              ? { ...normalizeMessageData(confirmedMessage) }
              : m
          ),
        };
      });
    } catch (error) {
      console.error("Failed to send message:", error);
      set((state: AppStore) => ({
        messages: state.messages.filter(
          (m: Message) => m.sequence !== tempMessage.sequence,
        ),
      }));
    }
  },

  editMessage: async (sequence: number, content: string) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;

    const originalContent = get().messages.find((m) => m.sequence === sequence)?.content;

    set((state: AppStore) => ({
      messages: state.messages.map((m: Message) =>
        m.sequence === sequence && m.isUser ? { ...m, content } : m,
      ),
    }));

    try {
      await client.editMessage(activeThreadId, sequence, content);
    } catch (error) {
      console.error("Failed to edit message:", error);
      if (originalContent) {
        set((state: AppStore) => ({
          messages: state.messages.map((m: Message) =>
            m.sequence === sequence && m.isUser ? { ...m, content: originalContent } : m,
          ),
        }));
      }
    }
  },

  restoreMessage: async (sequence: number) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;

    try {
      await client.unforgetMessage(activeThreadId, sequence);
      const messages = await client.getMessages(activeThreadId);
      set({ messages: (messages || []).map(normalizeMessageData) });
    } catch (error) {
      console.error("Failed to restore message:", error);
    }
  },

  deleteMessage: async (sequence: number) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;

    const originalMessages = get().messages;
    set((state: AppStore) => ({
      messages: state.messages.filter((m: Message) => m.sequence !== sequence),
    }));

    try {
      await client.forgetMessage(activeThreadId, sequence);
    } catch (error) {
      console.error("Failed to delete message:", error);
      set({ messages: originalMessages });
    }
  },

  focusAgent: (agentId: string | null) => {
    set({ activeAgentId: agentId });
  },

  setAgents: (newAgents: Agent[]) => {
    set((state) => {
      const threadIds = new Set(newAgents.map(a => a.threadId));
      const otherAgents = state.agents.filter(a => !threadIds.has(a.threadId));
      const updatedAgents = newAgents.map(newAgent => {
        const existingAgent = state.agents.find(a => a.id === newAgent.id);
        return {
          ...newAgent,
          tokensUsed: (newAgent.tokensUsed || existingAgent?.tokensUsed || 0)
        };
      });
      return { agents: [...otherAgents, ...updatedAgents] };
    });
  },

  updateAgentModel: (agentId: string, modelId: string) => {
    set((state) => ({
      agents: state.agents.map(a =>
        a.id === agentId ? { ...a, modelId } : a
      ),
    }));
  },

  loadThreads: async () => {
    try {
      const threads = await client.getThreads();
      const normalizedThreads = (threads || []).map(normalizeThreadData);
      set({ threads: normalizedThreads });
    } catch (error) {
      console.error("Failed to load threads:", error);
    }
  },

  deleteThread: async (threadId: string) => {
    const { activeThreadId, threads } = get();
    try {
      await client.deleteThread(threadId);
      set({
        threads: threads.filter((t) => t.id !== threadId),
        activeThreadId: activeThreadId === threadId ? null : activeThreadId,
        messages: activeThreadId === threadId ? [] : get().messages,
      });
    } catch (error) {
      console.error("Failed to delete thread:", error);
    }
  },

  updateFromEvent: (event: Event) => {
    switch (event.type) {
      case "thread_created": {
        const { threadId } = event as ThreadCreatedEvent;
        client.getThread(threadId).then((thread: Thread) => {
          const normalized = normalizeThreadData(thread);
          set((state: AppStore) => {
            const withoutDuplicate = state.threads.filter((t) => t.id !== normalized.id);
            return {
              threads: [normalized, ...withoutDuplicate].sort(
                (a: Thread, b: Thread) => b.checkpointedAt.getTime() - a.checkpointedAt.getTime(),
              ),
            };
          });
        }).catch((error: any) => console.error("Failed to load created thread:", error));
        break;
      }

      case "thread_updated" as any: {
        const payload = (event as any).payload;
        set((state: AppStore) => ({
          threads: state.threads.map((t) => t.id === event.threadId ? { ...t, ...payload } : t),
        }));
        break;
      }

      case "message_added": {
        const { message } = event as MessageAddedEvent;
        const type = (message as any).type;

        if (type === 'undo_deleted' || type === 'branch_deleted') {
          const seq = message.sequence;
          set((state: AppState) => ({
            messages: state.messages.filter(m => m.sequence <= seq)
          }));
          break;
        }

        const normalizedMessage = normalizeMessageData(message);
        set((state: AppStore) => {
          let newMessages = [...state.messages];
          const exists = newMessages.some((m: Message) => m.sequence === normalizedMessage.sequence);
          if (exists) {
            newMessages = newMessages.map(m => m.sequence === normalizedMessage.sequence ? normalizedMessage : m);
          } else {
            newMessages.push(normalizedMessage);
          }

          if (normalizedMessage.turnCount !== undefined) {
            newMessages = newMessages.filter(m => !(m.sequence < 0 && m.agentId === normalizedMessage.agentId && m.turnCount === normalizedMessage.turnCount));
          } else {
            newMessages = newMessages.filter(m => !(m.sequence < 0 && m.agentId === normalizedMessage.agentId));
          }

          return { messages: newMessages };
        });
        break;
      }

      case "agent_spawned" as any: {
        const spawnedEvent = event as any;
        set((state: AppStore) => {
          const existing = state.agents.find(a => a.id === spawnedEvent.agentId);
          const newAgent: Agent = {
            id: spawnedEvent.agentId,
            readableName: spawnedEvent.readableName,
            purpose: spawnedEvent.purpose,
            isLead: spawnedEvent.isLead || false,
            parentId: spawnedEvent.parentId,
            modelId: spawnedEvent.modelId,
            status: (existing?.status || "initializing") as any,
            turnCount: spawnedEvent.turnCount ?? existing?.turnCount ?? 1,
            objective: spawnedEvent.taskContext || existing?.objective || "",
            subagentIds: existing?.subagentIds || [],
            threadId: spawnedEvent.threadId,
            tokensUsed: 0,
          };

          let agents = state.agents.map((a) =>
            a.id === spawnedEvent.parentId
              ? { ...a, subagentIds: Array.from(new Set([...(a.subagentIds || []), spawnedEvent.agentId])) }
              : a
          );

          const existingIndex = agents.findIndex((a) => a.id === newAgent.id);
          if (existingIndex >= 0) {
            agents[existingIndex] = newAgent;
          } else {
            agents = [...agents, newAgent];
          }

          let updatedMessages = state.messages;
          const isGoalAgent = !spawnedEvent.isLead;
          const taskContext = spawnedEvent.taskContext;
          if (isGoalAgent && taskContext) {
            const objectiveMessage: Message = {
              sequence: generateTempSequence(),
              isUser: true,
              content: `Objective: ${taskContext}`,
              timestamp: new Date(),
              tokens: 0,
              type: 'response',
              agentId: spawnedEvent.agentId,
            };
            updatedMessages = [...state.messages, objectiveMessage];
          }

          if (spawnedEvent.parentId) {
            updatedMessages = updatedMessages.map((msg: Message) => {
              if (msg.agentId !== spawnedEvent.parentId || !msg.toolCalls) return msg;
              const toolCalls = msg.toolCalls.map((tc) => {
                if (tc.name === "agent_delegate" && tc.status === "running") {
                  const tcArgs = tc.args as any;
                  const getAgentId = (a: any) => typeof a === 'string' ? (() => { try { return JSON.parse(a).agentId; } catch { return null; } })() : a?.agentId;
                  
                  if (getAgentId(tcArgs) === spawnedEvent.readableName) {
                    return { 
                      ...tc, 
                      metadata: { ...(tc.metadata || {}), spawnedAgentId: spawnedEvent.agentId } 
                    };
                  }
                }
                return tc;
              });
              return { ...msg, toolCalls };
            });
          }

          return { agents, events: addEvent(state.events, event as any), messages: updatedMessages };
        });
        break;
      }

      case "agent_status_changed": {
        const { agentId, status, turnCount } = event as any;
        set((state: AppStore) => {
          const agents = state.agents.map((agent: Agent) => agent.id === agentId ? { ...agent, status: status as any } : agent);
          
          if (status === "working") {
             const existingIdx = state.messages.findLastIndex((m: Message) => 
               m.agentId === agentId && m.turnCount === turnCount && (m.isStreaming || m.type === "monologue")
             );
             
             if (existingIdx >= 0) return { agents };

             const streamingMessage: Message = {
               sequence: generateTempSequence(),
               isUser: false,
               content: "",
               timestamp: new Date(),
               tokens: 0,
               agentId,
               isStreaming: true,
               isMonologue: true,
               type: "monologue",
               turnCount: turnCount,
             };
             return {
               agents,
               messages: [...state.messages, streamingMessage],
             };
          }
          
          return { agents };
        });
        break;
      }

      case "agent_thinking": {
        const { agentId, content, turnCount } = event as any;
        set((state: AppStore) => {
          const streamingIdx = state.messages.findLastIndex((m: Message) => m.agentId === agentId && m.turnCount === turnCount);

          if (streamingIdx >= 0) {
            const updatedMessages = [...state.messages];
            const msg = updatedMessages[streamingIdx]!;
            updatedMessages[streamingIdx] = {
              ...msg,
              thinking: (msg.thinking || "") + content,
              isStreaming: true,
              isMonologue: true,
            };
            return { messages: updatedMessages, events: addEvent(state.events, event as any) };
          }

          const newMessage: Message = {
            sequence: generateTempSequence(),
            isUser: false,
            content: "",
            thinking: content,
            timestamp: new Date(),
            tokens: 0,
            agentId,
            isStreaming: true,
            isMonologue: true,
            type: "monologue",
            turnCount: turnCount,
          };
          return { messages: [...state.messages, newMessage], events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "agent_content": {
        const { agentId, content, turnCount, isComplete } = event as any;

        set((state: AppStore) => {
          const streamingIdx = state.messages.findLastIndex(
            (m: Message) => m.agentId === agentId && m.turnCount === turnCount
          );

          if (streamingIdx >= 0) {
            const updatedMessages = [...state.messages];
            const msg = updatedMessages[streamingIdx]!;
            updatedMessages[streamingIdx] = {
              ...msg,
              content: (typeof msg.content === "string" ? msg.content : "") + content,
              isStreaming: !isComplete,
              isMonologue: true,
            };
            return { messages: updatedMessages, events: addEvent(state.events, event as any) };
          }

          const newMessage: Message = {
            sequence: generateTempSequence(),
            isUser: false,
            content,
            timestamp: new Date(),
            tokens: 0,
            agentId,
            isStreaming: !isComplete,
            isMonologue: true,
            type: "monologue",
            turnCount: turnCount,
          };
          return { messages: [...state.messages, newMessage], events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "agent_provider_request": {
        const providerEvent = event as any;

        set((state: AppStore) => {
          const existingIdx = state.messages.findLastIndex(
            (m: Message) => m.type === "provider_request" && m.agentId === providerEvent.agentId && m.turnCount === providerEvent.turnCount
          );

          if (existingIdx >= 0) {
            const existing = state.messages[existingIdx]!;
            const updatedMessages = [...state.messages];
            updatedMessages[existingIdx] = {
              ...existing,
              providerRequest: providerEvent.request,
            };
            return { messages: updatedMessages, events: addEvent(state.events, event as any) };
          }

          const newMessage: Message = {
            sequence: generateTempSequence(),
            isUser: false,
            content: "",
            timestamp: new Date(providerEvent.timestamp),
            tokens: 0,
            agentId: providerEvent.agentId,
            isStreaming: false,
            isMonologue: false,
            type: "provider_request",
            providerRequest: providerEvent.request,
            turnCount: providerEvent.turnCount,
          };
          return {
            messages: [...state.messages, newMessage],
            events: addEvent(state.events, event as any),
          };
        });
        break;
      }

      case "tool_call_start" as any: {
        const toolStartEvent = event as any;
        const toolTurn = toolStartEvent.turnCount;
        set((state: AppStore) => {
          let targetIdx = state.messages.findLastIndex((m: Message) =>
            m.agentId === toolStartEvent.agentId &&
            m.turnCount === toolTurn
          );

          let newToolCall = {
            name: toolStartEvent.toolName,
            callId: toolStartEvent.callId || toolStartEvent.toolName + "-" + Date.now(),
            status: "running" as const,
            args: toolStartEvent.arguments,
          };

          if (targetIdx >= 0) {
            const updatedMessages = [...state.messages];
            const msg = updatedMessages[targetIdx]!;
            updatedMessages[targetIdx] = {
              ...msg,
              toolCalls: [...(msg.toolCalls || []), newToolCall],
              isStreaming: true,
            };
            return { messages: updatedMessages, events: addEvent(state.events, event as any) };
          } else {
            const placeholder: Message = {
              sequence: generateTempSequence(),
              isUser: false,
              content: "",
              timestamp: new Date(),
              tokens: 0,
              agentId: toolStartEvent.agentId,
              isStreaming: true,
              isMonologue: true,
              toolCalls: [newToolCall],
              type: "monologue",
              turnCount: toolTurn,
            };
            return { messages: [...state.messages, placeholder], events: addEvent(state.events, event as any) };
          }
        });
        break;
      }

      case "tool_call_end" as any: {
        const toolEndEvent = event as any;
        const toolTurn = toolEndEvent.turnCount;
        set((state: AppStore) => {
          let targetIdx = state.messages.findLastIndex((m: Message) =>
            m.agentId === toolEndEvent.agentId &&
            m.turnCount === toolTurn
          );

          if (targetIdx < 0) {
             targetIdx = state.messages.findLastIndex((m: Message) =>
               m.agentId === toolEndEvent.agentId && m.toolCalls && m.toolCalls.length > 0
             );
          }

          if (targetIdx >= 0) {
            const updatedMessages = [...state.messages];
            const toolCalls = [...(updatedMessages[targetIdx]!.toolCalls || [])];
            const runningIdx = toolCalls.findIndex((tc) => {
              if (toolEndEvent.callId && tc.callId) {
                return tc.callId === toolEndEvent.callId;
              }
              return tc.name === toolEndEvent.toolName && tc.status === "running";
            });
            if (runningIdx >= 0) {
              const hasError = toolEndEvent.success === false;
              const resultData = toolEndEvent.result;
              
              toolCalls[runningIdx] = {
                ...toolCalls[runningIdx]!,
                status: hasError ? "error" : "done",
                result: resultData?.output ?? resultData,
                summary: toolEndEvent.summary || "",
                error: toolEndEvent.error || resultData?.error || "",
                metadata: { ...(toolCalls[runningIdx]!.metadata || {}), ...(toolEndEvent.metadata || {}), ...(resultData?.metadata || {}) },
                durationMs: toolEndEvent.durationMs || toolEndEvent.executionTimeMs || 0,
              };
            }
            updatedMessages[targetIdx] = { ...updatedMessages[targetIdx]!, toolCalls, isStreaming: false };
            return { messages: updatedMessages, events: addEvent(state.events, event as any) };
          }

          return { events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "agent_terminated": {
        const terminatedEvent = event as any;
        set((state: AppStore) => {
          const lastIdx = state.messages.findLastIndex(
            (m: Message) => m.agentId === terminatedEvent.agentId
          );

          if (lastIdx === -1) {
            return { events: addEvent(state.events, event as any) };
          }

          const updatedMessages = [...state.messages];
          const msg = updatedMessages[lastIdx]!;

          updatedMessages[lastIdx] = {
            ...msg,
            isStreaming: false,
            type: "response",
            isMonologue: false,
          };

          return { messages: updatedMessages, events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "agent_metrics": {
        const { agentId, tokensUsed } = event as any;
        const { activeThreadId } = get();
        set((state: AppStore) => ({
          events: addEvent(state.events, event as any),
          threads: state.threads.map((t) => t.id === activeThreadId ? { ...t, tokensUsed: tokensUsed || t.tokensUsed || 0 } : t),
          agents: state.agents.map((a) => a.id === agentId ? { ...a, tokensUsed: tokensUsed || a.tokensUsed || 0 } : a),
        }));
        break;
      }

      case "agent_provider_error": {
        const providerErrorEvent = event as any;
        const errorMessage: Message = {
          sequence: generateTempSequence(),
          isUser: false,
          content: "",
          timestamp: new Date(),
          tokens: 0,
          agentId: providerErrorEvent.agentId,
          type: "provider_error",
          providerError: {
            error: providerErrorEvent.error,
            modelId: providerErrorEvent.modelId,
            providerId: providerErrorEvent.providerId,
          },
        };
        set((state: AppStore) => ({
          messages: [...state.messages, errorMessage],
          events: addEvent(state.events, event as any),
        }));
        break;
      }

      case "process_output": {
        const outputEvent = event as any;
        set((state: AppStore) => {
          const updatedMessages = [...state.messages];
          let targetIdx = updatedMessages.findIndex(m =>
            m.toolCalls?.some(tc => tc.callId === outputEvent.processId)
          );

          if (targetIdx === -1) {
            targetIdx = updatedMessages.findLastIndex(m =>
              m.agentId === outputEvent.agentId && m.isStreaming && m.toolCalls && m.toolCalls.length > 0
            );
          }

          if (targetIdx >= 0) {
            const msg = updatedMessages[targetIdx]!;
            const toolCallIdx = msg.toolCalls?.findIndex(tc => tc.callId === outputEvent.processId) ?? -1;

            if (toolCallIdx >= 0) {
              const toolCalls = [...(msg.toolCalls || [])];
              const tc = toolCalls[toolCallIdx]!;
              toolCalls[toolCallIdx] = {
                ...tc,
                streamingOutput: (tc.streamingOutput || "") + outputEvent.data
              };
              updatedMessages[targetIdx] = { ...msg, toolCalls };
            }
          }
          return { messages: updatedMessages, events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "process_exit": {
        const exitEvent = event as any;
        set((state: AppStore) => {
          const updatedMessages = [...state.messages];
          let targetIdx = updatedMessages.findIndex(m =>
            m.toolCalls?.some(tc => tc.callId === exitEvent.processId)
          );

          if (targetIdx >= 0) {
            const msg = updatedMessages[targetIdx]!;
            const toolCallIdx = msg.toolCalls?.findIndex(tc => tc.callId === exitEvent.processId) ?? -1;

            if (toolCallIdx >= 0) {
              const toolCalls = [...(msg.toolCalls || [])];
              const tc = toolCalls[toolCallIdx]!;
              toolCalls[toolCallIdx] = {
                ...tc,
                status: exitEvent.exitCode === 0 ? "done" : "error",
                exitCode: exitEvent.exitCode
              };
              updatedMessages[targetIdx] = { ...msg, toolCalls };
            }
          }
          return { messages: updatedMessages, events: addEvent(state.events, event as any) };
        });
        break;
      }

      case "tool_call_update" as any: {
        const updateEvent = event as any;
        const { callId, metadata, summary } = updateEvent;
        set((state: AppState) => {
          const messages = [...state.messages];
          for (let i = messages.length - 1; i >= 0; i--) {
            const msg = messages[i]!;
            if (msg.toolCalls) {
              const tcIndex = msg.toolCalls.findIndex((tc) => tc.callId === callId);
              if (tcIndex !== -1) {
                const newToolCalls = [...msg.toolCalls];
                newToolCalls[tcIndex] = {
                  ...newToolCalls[tcIndex]!,
                  metadata: { ...(newToolCalls[tcIndex]!.metadata || {}), ...metadata },
                  summary: summary || newToolCalls[tcIndex]!.summary,
                };
                messages[i] = { ...msg, toolCalls: newToolCalls };
                return { messages };
              }
            }
          }
          return state;
        });
        break;
      }

      case "agent_turn_complete" as any: {
        const { agentId, turnCount } = event as any;
        set((state: AppStore) => {
          const agents = state.agents.map((agent: Agent) => agent.id === agentId ? { ...agent, status: "idle" as any } : agent);
          const updatedMessages = state.messages.map((m: Message) => 
            (m.agentId === agentId && m.turnCount === turnCount) 
              ? { ...m, isStreaming: false } 
              : m
          );
          return { agents, messages: updatedMessages };
        });
        break;
      }

      default:
        break;
    }
  },

  toggleFleetSidebar: () => set((state) => ({ isFleetSidebarOpen: !state.isFleetSidebarOpen })),
  setSidebarTab: (tab: "threads" | "settings") => set({ sidebarTab: tab }),

  setRightSidebarTab: (tab: "fleet" | "todos" | "changes") => set({ rightSidebarTab: tab }),
  toggleRightSidebar: () => set((state) => ({ isRightSidebarOpen: !state.isRightSidebarOpen })),
  openRightSidebar: () => set({ isRightSidebarOpen: true }),
  closeRightSidebar: () => set({ isRightSidebarOpen: false }),

  fetchUserConfig: async () => {
    try {
      const config = await client.getUserConfig();
      set({ userConfig: config });
    } catch (error) {
      console.error("Failed to fetch user config:", error);
      set({ userConfig: null });
    }
  },

  updateUserConfig: async (config: UserConfig) => {
    try {
      const updated = await client.updateUserConfig(config);
      set({ userConfig: updated });
    } catch (error) {
      console.error("Failed to update user config:", error);
      throw error;
    }
  },
}));

export const selectFilteredMessages = (state: AppStore): Message[] => {
  const { messages, activeAgentId, activeThreadId, threads, agents } = state;
  let targetAgentId = activeAgentId;
  if (!targetAgentId && activeThreadId) {
    const activeThread = threads.find((t) => t.id === activeThreadId);
    if (activeThread?.leadAgentId) targetAgentId = activeThread.leadAgentId;
  }
  const filtered = messages.filter((msg) => {
    if (msg.isUser) return true;
    if (targetAgentId && msg.agentId === targetAgentId) return true;
    if (!activeAgentId && msg.type === "response") {
      const leadAgent = agents.find(a => a.isLead);
      return !leadAgent || msg.agentId === leadAgent.id;
    }
    if (!activeAgentId && msg.isStreaming && msg.agentId === targetAgentId) return true;
    return false;
  });
  return [...filtered].sort((a, b) => {
    const aTime = a.timestamp instanceof Date ? a.timestamp.getTime() : new Date(a.timestamp).getTime();
    const bTime = b.timestamp instanceof Date ? b.timestamp.getTime() : new Date(b.timestamp).getTime();
    const timeDiff = aTime - bTime;
    return timeDiff !== 0 ? timeDiff : a.sequence - b.sequence;
  });
};

export const selectWorkingAgentId = (state: AppStore): string | null => {
  if (!state.activeThreadId) return null;
  const workingAgent = state.agents.find(a => a.threadId === state.activeThreadId && a.status === 'working');
  return workingAgent?.id || null;
};

export const selectAgentMetrics = (state: AppStore): AgentMetricsEvent[] => {
  return state.events.filter((e) => e.type === "agent_metrics") as AgentMetricsEvent[];
};

export const selectAgentLiveActivities = (agentId: string) => (state: AppStore): LiveActivity[] => {
  const agent = state.agents.find(a => a.id === agentId);
  if (!agent || agent.status !== "working") return [];
  
  const agentMessages = state.messages
    .filter((m: Message) => m.agentId === agentId && m.toolCalls)
    .slice(-3);
  
  const activities: LiveActivity[] = [];
  for (const msg of agentMessages) {
    for (const tc of msg.toolCalls || []) {
      if (tc.status === "running" || tc.status === "preparing") {
        activities.push({
          toolName: tc.name,
          description: tc.summary || tc.name,
          status: tc.status,
          startedAt: msg.timestamp,
        });
      }
    }
  }
  return activities;
};

export default useAppStore;
export { useAppStore };
