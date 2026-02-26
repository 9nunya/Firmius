import { create } from "zustand";
import { sseClient } from "@firmius/shared/sse";
import type {
  ThreadResponse as Thread,
  Message,
  AgentResponse as Agent,
  AgentStatus,
  APIError as BackendAPIError,
  ThreadGenerationOptions,
  CreateThreadRequest,
  ProviderInfo,
} from "@firmius/shared/api";
import { client, APIError, isAPIError } from "@firmius/shared/api";

function normalizeThread(thread: Thread & { checkpointedAt?: Date | string }): Thread {
  return {
    ...thread,
    checkpointedAt: thread.checkpointedAt ? new Date(thread.checkpointedAt) : new Date(),
  };
}

function normalizeMessage(message: Message & { timestamp?: Date | string | number }): Message {
  return {
    ...message,
    timestamp: message.timestamp instanceof Date ? message.timestamp : new Date(message.timestamp),
  };
}

type ConnectionStatus = "connected" | "disconnected" | "connecting";

interface TuiSpecificState {
  inputBuffer: string;
  cursorPosition: number;
  isCommandPaletteOpen: boolean;
  activeModal: "threads" | "agents" | "wizard" | "models" | "fleet" | null;
  navigationHistory: string[];
  jumpMode: boolean;
  jumpCodes: Record<string, number>;
  collapsedTurns: Set<string>;
}

interface AppState extends TuiSpecificState {
  threads: Thread[];
  activeThreadId: string | null;
  messages: Message[];
  agents: Agent[];
  activeAgentId: string | null;
  connectionStatus: ConnectionStatus;
  isLoading: boolean;
  error: BackendAPIError | null;
  events: Record<string, unknown>[];
  providers: ProviderInfo[];
  messageQueue: (string | unknown[])[];
  sseUnsubscribe: (() => void) | null;
}

interface AppActions {
  selectThread: (threadId: string) => Promise<void>;
  createThread: (data: CreateThreadRequest) => Promise<void>;
  sendMessage: (content: string | unknown[]) => Promise<void>;
  interruptThread: () => Promise<void>;
  editMessage: (sequence: number, content: string) => Promise<void>;
  branchThread: (sequence: number, content: string) => Promise<void>;
  deleteMessage: (sequence: number) => Promise<void>;
  restoreMessage: (sequence: number) => Promise<void>;
  focusAgent: (agentId: string | null) => void;
  loadThreads: () => Promise<void>;
  loadProviders: () => Promise<void>;
  updateThreadSettings: (settings: ThreadGenerationOptions) => Promise<void>;
  updateFromEvent: (event: Record<string, unknown>) => void;
  setInputBuffer: (buffer: string) => void;
  setCursorPosition: (position: number) => void;
  setCommandPaletteOpen: (isOpen: boolean) => void;
  setActiveModal: (modal: AppState["activeModal"]) => void;
  addToNavigationHistory: (input: string) => void;
  setJumpMode: (enabled: boolean) => void;
  setJumpCodes: (codes: Record<string, number>) => void;
  toggleTurnCollapse: (turnId: string) => void;
}

type AppStore = AppState & AppActions;

const useAppStore = create<AppStore>((set, get) => ({
  threads: [],
  activeThreadId: null,
  messages: [],
  agents: [],
  activeAgentId: null,
  connectionStatus: "disconnected",
  isLoading: false,
  error: null,
  events: [],
  providers: [],
  messageQueue: [],
  sseUnsubscribe: null,

  inputBuffer: "",
  cursorPosition: 0,
  isCommandPaletteOpen: false,
  activeModal: null,
  navigationHistory: [],
  jumpMode: false,
  jumpCodes: {},
  collapsedTurns: new Set(),

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
    set({ isLoading: true });
    try {
      await client.updateThreadSettings(activeThreadId, settings);
      const updatedThread = await client.getThread(activeThreadId);
      set((state) => ({
        threads: state.threads.map((t) =>
          t.id === activeThreadId ? normalizeThread(updatedThread) : t
        ),
        isLoading: false,
        error: null,
      }));
    } catch (error: unknown) {
      console.error("Failed to update thread settings:", error);
      set({
        isLoading: false,
        error: isAPIError(error) ? error : new APIError("Failed to update settings"),
      });
    }
  },

  branchThread: async (sequence: number, content: string) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;

    set({ isLoading: true });
    try {
      await client.branchThread(activeThreadId, sequence, content);
      set((state) => ({
        messages: state.messages.filter((m) => m.sequence <= sequence),
      }));
    } catch (error) {
      console.error("Failed to branch thread:", error);
      set({ isLoading: false });
    }
  },

  createThread: async (data: CreateThreadRequest) => {
    set({ isLoading: true });
    try {
      const thread = await client.createThread({
        purpose: "orchestrator",
        objective: "",
        hostConfig: data.hostConfig,
        rootCwd: data.rootCwd,
        workType: data.workType,
        generationOptions: data.generationOptions,
      });
      const normalizedThread = normalizeThread(thread);
      set((state) => ({
        threads: [normalizedThread, ...state.threads],
        activeThreadId: normalizedThread.id,
        isLoading: false,
      }));
      await get().selectThread(normalizedThread.id);
    } catch (error) {
      console.error("Failed to create thread:", error);
      set({ isLoading: false });
      throw error;
    }
  },

  selectThread: async (threadId: string) => {
    const { sseUnsubscribe: oldUnsubscribe } = get();
    if (oldUnsubscribe) oldUnsubscribe();

    const thread = get().threads.find(t => t.id === threadId);
    let initialAgents: Agent[] = [];

    if (thread?.leadAgentId) {
        initialAgents = [
          {
            id: thread.leadAgentId,
            readableName: "Lead Agent",
            purpose: "Assistant",
            isLead: true,
            status: "idle",
            modelId: thread.modelId || "default",
            threadId: threadId,
            turnCount: 0,
            objective: "",
            subagentIds: [],
          },
        ];
    }

    set({ 
        activeThreadId: threadId, 
        messages: [], 
        agents: initialAgents, 
        activeAgentId: initialAgents[0]?.id || null,
        connectionStatus: 'connecting', 
        collapsedTurns: new Set(),
        isLoading: true
    });

    try {
      const unsubMsg = sseClient.onParsedMessage((event) => {
        if (event) get().updateFromEvent(event as Record<string, unknown>);
      });
      const unsubStatus = sseClient.onStatusChange((status) => {
        set({ connectionStatus: status === 'connected' ? 'connected' : status === 'connecting' ? 'connecting' : 'disconnected' });
      });

      set({ sseUnsubscribe: () => { unsubMsg(); unsubStatus(); sseClient.disconnect(); } });
      sseClient.connect(threadId);

      const [messages, agents] = await Promise.all([
        client.getMessages(threadId),
        client.getAgents(threadId),
      ]);

      const normalizedMessages = (messages || []).map(normalizeMessage);
      const completedTurnIds = new Set<string>();
      let currentTurnId: string | null = null;
      normalizedMessages.forEach(msg => {
          if (msg.isUser) currentTurnId = `turn-${msg.sequence}`;
          else if (msg.type === 'response' && !msg.isStreaming && currentTurnId) completedTurnIds.add(currentTurnId);
      });

      set({
        messages: normalizedMessages,
        agents: (agents && agents.length > 0) ? agents : initialAgents,
        activeAgentId: get().activeAgentId || (agents && agents.length > 0 ? (agents.find(a => a.isLead)?.id || agents[0]?.id) : initialAgents[0]?.id) || null,
        isLoading: false,
        collapsedTurns: completedTurnIds
      });
    } catch (error) {
      console.error("Failed to load thread:", error);
      set({ isLoading: false });
    }
  },

  sendMessage: async (content: string | unknown[]) => {
    const { activeThreadId, isLoading, messageQueue } = get();
    if (!activeThreadId) return;

    if (isLoading) {
      set({ messageQueue: [...messageQueue, content] });
      return;
    }

    const now = Date.now();
    const tempMessage: Message = {
      sequence: 1e15 + now,
      isUser: true,
      content,
      timestamp: new Date(),
      tokens: 0,
      type: "response",
    };

    set((state) => ({
      messages: [...state.messages, tempMessage],
      isLoading: true,
    }));

    try {
      const confirmedMessage = await client.sendMessage(activeThreadId, content);
      set((state) => {
        const alreadyExists = state.messages.some((m) => m.sequence === confirmedMessage.sequence);
        if (alreadyExists) {
          return {
            messages: state.messages.filter((m) => m.sequence !== tempMessage.sequence),
          };
        } else {
          return {
            messages: state.messages.map((m) =>
              m.sequence === tempMessage.sequence ? normalizeMessage(confirmedMessage) : m
            ),
          };
        }
      });
    } catch (error) {
      console.error("Failed to send message:", error);
      set((state) => ({
        messages: state.messages.filter((m) => m.sequence !== tempMessage.sequence),
        isLoading: false,
      }));

      const { messageQueue: currentQueue } = get();
      if (currentQueue.length > 0) {
        const nextMessage = currentQueue[0];
        if (nextMessage) {
          const remainingQueue = currentQueue.slice(1);
          set({ messageQueue: remainingQueue });
          setTimeout(() => {
            get().sendMessage(nextMessage);
          }, 0);
        }
      }
    }
  },

  editMessage: async (sequence: number, content: string) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;
    const originalContent = get().messages.find((m) => m.sequence === sequence)?.content;
    set((state) => ({
      messages: state.messages.map((m) =>
        m.sequence === sequence && m.isUser ? { ...m, content } : m
      ),
    }));
    try {
      await client.editMessage(activeThreadId, sequence, content);
    } catch (error) {
      console.error("Failed to edit message:", error);
      if (originalContent) {
        set((state) => ({
          messages: state.messages.map((m) =>
            m.sequence === sequence && m.isUser ? { ...m, content: originalContent } : m
          ),
        }));
      }
    }
  },

  restoreMessage: async (sequence: number) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;
    set({ isLoading: true });
    try {
      await client.unforgetMessage(activeThreadId, sequence);
      const messages = await client.getMessages(activeThreadId);
      set({
        messages: (messages || []).map(normalizeMessage),
        isLoading: false,
      });
    } catch (error) {
      console.error("Failed to restore message:", error);
      set({ isLoading: false });
    }
  },

  deleteMessage: async (sequence: number) => {
    const { activeThreadId } = get();
    if (!activeThreadId) return;
    const originalMessages = get().messages;
    set((state) => ({
      messages: state.messages.filter((m) => m.sequence !== sequence),
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

  loadThreads: async () => {
    set({ isLoading: true });
    try {
      const threads = await client.getThreads();
      const normalizedThreads = (threads || []).map(normalizeThread);
      set({
        threads: normalizedThreads,
        isLoading: false,
      });
    } catch (error) {
      console.error("Failed to load threads:", error);
      set({ isLoading: false });
    }
  },

  updateFromEvent: (event: Record<string, unknown>) => {
    const type = event.type as string;
    const agentId = event.agentId as string | undefined;
    const data = event.data as Record<string, unknown> | undefined;
    const { activeThreadId } = get();
    const threadId = event.threadId as string | null;
    if (threadId && threadId !== activeThreadId) return;

    switch (type) {
      case "thread_created": {
        const payload = data;
        if (!payload) return;
        client
          .getThread(payload.threadId as string)
          .then((thread) => {
            const normalized = normalizeThread(thread);
            set((state) => {
              const withoutDuplicate = state.threads.filter((t) => t.id !== normalized.id);
              return {
                threads: [normalized, ...withoutDuplicate].sort(
                  (a, b) => b.checkpointedAt.getTime() - a.checkpointedAt.getTime()
                ),
              };
            });
          })
          .catch((error) => console.error("Failed to load created thread:", error));
        break;
      }

      case "message_added": {
        const payload = data;
        if (!payload) return;
        const msg = payload.message as Record<string, unknown> | undefined;
        if (!msg) return;
        const normalizedMessage = normalizeMessage({
          sequence: msg.sequence as number,
          isUser: msg.isUser as boolean,
          content: msg.content as string | unknown[],
          timestamp: msg.timestamp instanceof Date ? msg.timestamp : new Date(msg.timestamp as string | number),
          tokens: msg.tokens as number,
          type: msg.type as "response" | "monologue" | "provider_request" | "provider_error",
          isMonologue: msg.isMonologue as boolean | undefined,
          thinking: msg.thinking as string | undefined,
          agentId: msg.agentId as string | undefined,
        });
        set((state) => {
          const exists = state.messages.some((m) => m.sequence === normalizedMessage.sequence);
          if (exists) return state;
          
          let nextCollapsed = state.collapsedTurns;
          if (normalizedMessage.type === 'response' && !normalizedMessage.isStreaming) {
              const lastUserMsg = [...state.messages].reverse().find(m => m.isUser);
              if (lastUserMsg) {
                  nextCollapsed = new Set(state.collapsedTurns);
                  nextCollapsed.add(`turn-${lastUserMsg.sequence}`);
              }
          }

          return {
            messages: [...state.messages, normalizedMessage],
            collapsedTurns: nextCollapsed
          };
        });
        break;
      }

      case "agent_spawned": {
        const payload = data;
        if (!payload) return;
        set((state) => {
          const aId = String(payload.agentId || agentId || "");
          const existing = state.agents.find((a) => a.id === aId);
          const subagentIds = existing?.subagentIds ?? [];

          const newAgent: Agent = {
            id: aId,
            readableName: String(payload.readableName || ""),
            purpose: String(payload.purpose || ""),
            isLead: Boolean(payload.isLead),
            parentId: payload.parentId ? String(payload.parentId) : undefined,
            modelId: payload.modelId ? String(payload.modelId) : undefined,
            status: existing?.status || "initializing",
            turnCount: existing?.turnCount ?? 1,
            objective: String(payload.objective || existing?.objective || ""),
            subagentIds,
            threadId: threadId || activeThreadId || "",
          };

          let agents = state.agents.map((a) =>
            a.id === payload.parentId
              ? {
                  ...a,
                  subagentIds: Array.from(new Set([...(a.subagentIds || []), aId])),
                }
              : a
          );

          const existingIndex = agents.findIndex((a) => a.id === newAgent.id);
          if (existingIndex >= 0) {
            const updatedAgents = [...agents];
            updatedAgents[existingIndex] = newAgent;
            agents = updatedAgents;
          } else {
            agents = [...agents, newAgent];
          }

          return {
            agents,
            events: [...state.events, event],
          };
        });
        break;
      }

      case "agent_status_changed":
      case "agent_status": {
        const payload = data;
        const status = (payload?.status as AgentStatus) || "idle";

        if (status === "working") {
          const now = Date.now();
          const streamingMessage: Message = {
            sequence: 1e15 + now + 1,
            isUser: false,
            content: "",
            timestamp: new Date(),
            tokens: 0,
            agentId,
            isStreaming: true,
            isMonologue: true,
            type: "monologue",
          };
          set((state) => {
            const lastMsg = state.messages[state.messages.length - 1];
            const isCompatible =
              lastMsg &&
              lastMsg.isStreaming &&
              lastMsg.agentId === agentId &&
              lastMsg.type === "monologue" &&
              (!lastMsg.toolCalls || lastMsg.toolCalls.length === 0);

            if (isCompatible) {
              return {
                agents: state.agents.map((agent) =>
                  agent.id === agentId ? { ...agent, status } : agent
                ),
                isLoading: true,
              };
            }

            const messages = state.messages.map((m) =>
              m.isStreaming && m.agentId === agentId ? { ...m, isStreaming: false } : m
            );

            return {
              agents: state.agents.map((agent) =>
                agent.id === agentId ? { ...agent, status } : agent
              ),
              isLoading: true,
              messages: [...messages, streamingMessage],
            };
          });
        } else {
          set((state) => ({
            agents: state.agents.map((agent) =>
              agent.id === agentId ? { ...agent, status } : agent
            ),
            isLoading: false,
          }));
        }
        break;
      }

      case "agent_thinking": {
        const payload = data;
        if (!payload) return;
        const thought = String(payload.thought || "");
        set((state) => {
          const lastMsg = state.messages[state.messages.length - 1];
          const isCompatible =
            lastMsg &&
            lastMsg.isStreaming &&
            lastMsg.agentId === agentId &&
            (!lastMsg.content || lastMsg.content === "") &&
            (!lastMsg.toolCalls || lastMsg.toolCalls.length === 0);

          if (isCompatible) {
            const updatedMessages = [...state.messages];
            const idx = updatedMessages.length - 1;
            const current = updatedMessages[idx]!;
            updatedMessages[idx] = {
              ...current,
              thinking: (current.thinking || "") + thought,
              isStreaming: true,
              isMonologue: true,
              type: "monologue"
            };
            return { messages: updatedMessages };
          } else {
            const now = Date.now();
            const newMessage: Message = {
              sequence: 1e15 + now,
              isUser: false,
              content: "",
              thinking: thought,
              timestamp: new Date(),
              tokens: 0,
              agentId,
              isStreaming: true,
              isMonologue: true,
              type: "monologue",
            };
            const messages = state.messages.map((m) =>
              m.isStreaming && m.agentId === agentId ? { ...m, isStreaming: false } : m
            );
            return { messages: [...messages, newMessage] };
          }
        });
        break;
      }

      case "agent_content": {
        const payload = data;
        if (!payload) return;
        const content = String(payload.content || "");
        set((state) => {
          const lastMsg = state.messages[state.messages.length - 1];
          const isCompatible =
            lastMsg &&
            lastMsg.isStreaming &&
            lastMsg.agentId === agentId &&
            (!lastMsg.toolCalls || lastMsg.toolCalls.length === 0);

          if (isCompatible) {
            const updatedMessages = [...state.messages];
            const idx = updatedMessages.length - 1;
            const current = updatedMessages[idx]!;
            updatedMessages[idx] = {
              ...current,
              content: (typeof current.content === "string" ? current.content : "") + content,
              isStreaming: true,
              isMonologue: true,
            };
            return { messages: updatedMessages };
          } else {
            const now = Date.now();
            const newMessage: Message = {
              sequence: 1e15 + now,
              isUser: false,
              content: content,
              timestamp: new Date(),
              tokens: 0,
              agentId,
              isStreaming: true,
              isMonologue: true,
              type: "monologue",
            };
            const messages = state.messages.map((m) =>
              m.isStreaming && m.agentId === agentId ? { ...m, isStreaming: false } : m
            );
            return { messages: [...messages, newMessage] };
          }
        });
        break;
      }

      case "tool_call_start":
      case "tool_call_started": {
        const payload = data;
        if (!payload) return;
        set((state) => {
          const now = Date.now();
          const toolCallMsg: Message = {
              sequence: 1e15 + now,
              isUser: false,
              content: "",
              timestamp: new Date(),
              tokens: 0,
              agentId,
              isStreaming: false,
              isMonologue: true,
              type: "monologue",
              toolCalls: [{
                name: String(payload.toolName || payload.name || ""),
                callId: String(payload.callId || (payload.toolName || payload.name || "") + "-" + now),
                status: "running" as const,
                args: payload.arguments as Record<string, unknown> | undefined,
              }]
          };
          
          const messages = state.messages.map((m) =>
            m.isStreaming && m.agentId === agentId ? { ...m, isStreaming: false } : m
          );
          
          return { 
              messages: [...messages, toolCallMsg],
              isLoading: true 
          };
        });
        break;
      }

      case "tool_call_end":
      case "tool_call_completed": {
        const payload = data;
        if (!payload) return;
        set((state) => {
          const name = String(payload.toolName || payload.name || "");
          const updatedMessages = state.messages.map(m => {
              if (m.agentId === agentId && m.toolCalls) {
                  const tcIdx = m.toolCalls.findIndex(tc => tc.name === name && tc.status === 'running');
                  if (tcIdx >= 0) {
                      const newTCs = [...m.toolCalls];
                      newTCs[tcIdx] = {
                          ...newTCs[tcIdx]!,
                          status: 'done',
                          result: String(payload.result ?? "")
                      };
                      return { ...m, toolCalls: newTCs };
                  }
              }
              return m;
          });
          return { messages: updatedMessages };
        });
        break;
      }

      case "agent_terminated": {
        if (activeThreadId) {
          client
            .getMessages(activeThreadId)
            .then((refreshedMessages) => {
              set({
                messages: (refreshedMessages || []).map(normalizeMessage),
                isLoading: false,
              });
            })
            .catch((err) => {
              console.error("Failed to refresh messages after termination:", err);
              set((state) => ({
                messages: state.messages.map((m) =>
                  m.isStreaming && m.agentId === agentId
                    ? { ...m, isStreaming: false, type: "response", isMonologue: false }
                    : m
                ),
                isLoading: false,
              }));
            });
        } else {
          set({ isLoading: false });
        }
        break;
      }

      case "agent_metrics": {
        const payload = data;
        if (!payload) return;
        set((state) => ({
          threads: state.threads.map((t) =>
            t.id === activeThreadId
              ? { ...t, tokensUsed: (payload.tokensUsed as number) || t.tokensUsed || 0 }
              : t
          ),
        }));
        break;
      }

      default:
        break;
    }
  },

  setInputBuffer: (buffer) => set({ inputBuffer: buffer }),
  setCursorPosition: (position) => set({ cursorPosition: position }),
  setCommandPaletteOpen: (isOpen) => set({ isCommandPaletteOpen: isOpen }),
  setActiveModal: (modal) => set({ activeModal: modal }),
  addToNavigationHistory: (input) =>
    set((state) => ({
      navigationHistory: Array.from(new Set([input, ...state.navigationHistory])).slice(0, 50),
    })),
  setJumpMode: (enabled) => set({ jumpMode: enabled }),
  setJumpCodes: (codes) => set({ jumpCodes: codes }),
  toggleTurnCollapse: (turnId: string) => set((state) => {
      const next = new Set(state.collapsedTurns);
      if (next.has(turnId)) next.delete(turnId);
      else next.add(turnId);
      return { collapsedTurns: next };
  }),
}));

export const selectFilteredMessages = (state: AppStore): Message[] => {
  const { messages, activeAgentId, activeThreadId, threads } = state;
  let targetAgentId = activeAgentId;
  if (!targetAgentId && activeThreadId) {
    const activeThread = threads.find((t) => t.id === activeThreadId);
    if (activeThread?.leadAgentId) {
      targetAgentId = activeThread.leadAgentId;
    }
  }
  const filtered = messages.filter((msg) => {
    if (msg.isUser) return true;
    if (targetAgentId && msg.agentId === targetAgentId) return true;
    if (!activeAgentId && msg.type === "response") return true;
    if (!activeAgentId && msg.isStreaming && msg.agentId === targetAgentId) return true;
    return false;
  });
  return [...filtered].sort((a, b) => a.sequence - b.sequence);
};

export default useAppStore;