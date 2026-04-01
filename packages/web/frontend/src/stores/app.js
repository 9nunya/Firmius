import { derived, get, writable } from "svelte/store";
import { api } from "../lib/api.js";
import { applyTheme } from "../lib/theme.js";

function freshLive() {
  return {
    thinkingByAgent: {},
    textByAgent: {},
    providerWaiting: {},
    retryByAgent: {},
    queued: [],
    pendingUserMessages: [],
    tools: {},
    processById: {},
    streamState: "connecting",
    streamMessage: "",
    lastEventId: 0,
  };
}

function ensureTool(live, toolCallId) {
  if (!live.tools[toolCallId]) {
    live.tools[toolCallId] = {
      id: toolCallId,
      agentId: "",
      name: "",
      args: "",
      phase: "preparing",
      result: "",
      processId: "",
      subagentId: "",
      startedAt: Date.now(),
    };
  }
  return live.tools[toolCallId];
}

function createAppStore() {
  const snapshot = writable(null);
  const themes = writable([]);
  const themeName = writable("");
  const live = writable(freshLive());
  const error = writable("");

  let streamController = null;
  let streamToken = 0;
  let refreshTimer = null;

  function setConnection(state, message = "") {
    live.update((current) => ({ ...current, streamState: state, streamMessage: message }));
  }

  function scheduleRefresh(delay = 120) {
    clearTimeout(refreshTimer);
    refreshTimer = setTimeout(() => {
      refreshState({ keepLive: true }).catch((err) => error.set(err.message));
    }, delay);
  }

  function applyEvent(payload) {
    if (!payload || typeof payload !== "object") return;
    live.update((current) => {
      current.lastEventId = Math.max(current.lastEventId || 0, Number(payload.eventId || 0));
      switch (payload.type) {
        case "agentThinking":
          current.thinkingByAgent[payload.agentId] =
            (current.thinkingByAgent[payload.agentId] || "") + (payload.delta || "");
          break;
        case "agentText":
          current.textByAgent[payload.agentId] =
            (current.textByAgent[payload.agentId] || "") + (payload.delta || "");
          break;
        case "agentProviderWaiting":
          current.providerWaiting[payload.agentId] = true;
          break;
        case "agentRetrying":
          current.retryByAgent[payload.agentId] = `retry ${payload.attempt}/${payload.maxAttempts} in ${payload.delayMs}ms`;
          break;
        case "agentRetryFailed":
          current.retryByAgent[payload.agentId] = `retry failed: ${payload.reason || payload.httpStatus || "unknown"}`;
          break;
        case "messageQueued":
          current.pendingUserMessages = current.pendingUserMessages.filter(
            (entry) => entry.text !== payload.text,
          );
          current.queued = [...current.queued, payload];
          break;
        case "messageDequeued":
          current.queued = current.queued.filter((entry) => entry.messageId !== payload.messageId);
          break;
        case "userMessageSent":
          current.pendingUserMessages = current.pendingUserMessages.filter(
            (entry) => entry.text !== payload.text,
          );
          scheduleRefresh(40);
          break;
        case "agentToolCallChunk": {
          const tool = ensureTool(current, payload.toolCallId);
          tool.agentId = payload.agentId || tool.agentId;
          tool.name += payload.nameDelta || "";
          tool.args += payload.argsDelta || "";
          if (tool.args) tool.phase = "called";
          break;
        }
        case "agentToolCall": {
          const tool = ensureTool(current, payload.toolCallId);
          tool.agentId = payload.agentId || tool.agentId;
          tool.name = payload.toolName || tool.name;
          tool.args = payload.toolArgs || tool.args;
          tool.phase = "called";
          break;
        }
        case "agentProcessSpawned": {
          const tool = ensureTool(current, payload.toolCallId);
          tool.agentId = payload.agentId || tool.agentId;
          tool.processId = payload.processId || "";
          tool.phase = "running";
          current.processById[payload.processId] = {
            stdout: "",
            stderr: "",
            finished: false,
            exitCode: null,
            durationMs: null,
          };
          break;
        }
        case "agentProcessOutput": {
          const process = current.processById[payload.processId] || {
            stdout: "",
            stderr: "",
            finished: false,
            exitCode: null,
            durationMs: null,
          };
          if (payload.isStderr) {
            process.stderr += payload.output || "";
          } else {
            process.stdout += payload.output || "";
          }
          process.finished = Boolean(payload.finished);
          process.exitCode = payload.exitCode;
          process.durationMs = payload.durationMs;
          current.processById[payload.processId] = process;
          break;
        }
        case "agentTurnCompleted": {
          const turn = payload.turn || {};
          for (const message of turn.messages || []) {
            for (const part of message.parts || message.content || []) {
              if (part.type === "toolResult" && part.toolCallId) {
                const tool = ensureTool(current, part.toolCallId);
                tool.result = part.result || "";
                tool.subagentId = part.subagentId || tool.subagentId;
                tool.phase = part.success ? "finished" : "error";
              }
            }
          }
          current.thinkingByAgent[payload.agentId] = "";
          current.textByAgent[payload.agentId] = "";
          delete current.providerWaiting[payload.agentId];
          delete current.retryByAgent[payload.agentId];
          current.streamMessage = "";
          scheduleRefresh(80);
          break;
        }
        case "agentInterrupted":
        case "agentFinished":
        case "agentError":
        case "threadChanged":
        case "threadMetadataUpdated":
        case "modelSwitched":
        case "modelsRefreshed":
        case "permissionEscalationRequest":
        case "permissionEscalationResolved":
        case "configUpdated":
        case "historyUndone":
        case "planCreated":
        case "planUpdated":
        case "planActivated":
        case "chunkAdded":
        case "chunkUpdated":
        case "chunkAssigned":
        case "chunkStatusChanged":
          if (payload.agentId) {
            current.thinkingByAgent[payload.agentId] = "";
            current.textByAgent[payload.agentId] = "";
            delete current.providerWaiting[payload.agentId];
            delete current.retryByAgent[payload.agentId];
            for (const tool of Object.values(current.tools)) {
              if (tool.agentId === payload.agentId && tool.phase !== "finished" && tool.phase !== "error") {
                tool.phase = payload.type === "agentError" ? "error" : tool.phase;
              }
            }
          }
          current.streamMessage = "";
          scheduleRefresh(60);
          break;
      }
      return current;
    });
  }

  function disconnectStream() {
    streamToken += 1;
    if (streamController) {
      streamController.abort();
      streamController = null;
    }
  }

  function parseEventStream(body) {
    const records = [];
    let eventType = "";
    let dataLines = [];
    let lastEventId = "";

    for (const line of String(body || "").split("\n")) {
      if (!line.trim()) {
        if (dataLines.length) {
          records.push({
            event: eventType || "message",
            data: dataLines.join("\n"),
            id: lastEventId,
          });
        }
        eventType = "";
        dataLines = [];
        lastEventId = "";
        continue;
      }
      if (line.startsWith("event:")) {
        eventType = line.slice(6).trim();
      } else if (line.startsWith("data:")) {
        dataLines.push(line.slice(5).trimStart());
      } else if (line.startsWith("id:")) {
        lastEventId = line.slice(3).trim();
      }
    }

    return records;
  }

  async function streamLoop(threadId, token) {
    while (token === streamToken) {
      const current = get(snapshot);
      const liveState = get(live);
      const query = new URLSearchParams({
        threadId,
        since: String(liveState.lastEventId || current?.latestEventId || 0),
      });
      streamController = new AbortController();

      try {
        const response = await fetch(`/api/events/stream?${query.toString()}`, {
          signal: streamController.signal,
          headers: { Accept: "text/event-stream" },
          cache: "no-store",
        });
        const body = await response.text();
        if (token !== streamToken) return;
        const events = parseEventStream(body);
        setConnection("connected", events.length ? "streaming" : "");
        for (const event of events) {
          if (event.event === "app" && event.data) {
            applyEvent(JSON.parse(event.data));
          }
        }
      } catch (err) {
        if (token !== streamToken) return;
        setConnection("disconnected", "reconnecting");
        await new Promise((resolve) => setTimeout(resolve, 1200));
      }
    }
  }

  function connectStream() {
    disconnectStream();
    const current = get(snapshot);
    const liveState = get(live);
    if (!current?.currentThreadId) {
      setConnection("connecting", "no active thread");
      return;
    }
    setConnection("connecting", "opening stream");
    const token = ++streamToken;
    streamLoop(current.currentThreadId, token).catch((err) => error.set(err.message));
  }

  async function loadThemes() {
    const response = await api.themes();
    themes.set(response.themes || []);
    const preferred =
      localStorage.getItem("firmius-web-theme") || response.preferredTheme || "";
    const available = response.themes || [];
    const next = available.find((theme) => theme.name === preferred) || available[0];
    if (next) {
      themeName.set(next.name);
      localStorage.setItem("firmius-web-theme", next.name);
      applyTheme(next);
    }
  }

  async function refreshState({ keepLive = false } = {}) {
    const response = await api.state();
    snapshot.set(response);
    themes.set(response.themes || []);
    if (!keepLive) {
      live.set(freshLive());
    } else {
      live.update((current) => ({
        ...current,
        lastEventId: Math.max(current.lastEventId || 0, Number(response.latestEventId || 0)),
      }));
    }

    const selectedTheme =
      get(themeName) || response.preferredTheme || localStorage.getItem("firmius-web-theme") || "";
    const availableThemes = response.themes || [];
    const theme = availableThemes.find((item) => item.name === selectedTheme) || availableThemes[0];
    if (theme) {
      themeName.set(theme.name);
      localStorage.setItem("firmius-web-theme", theme.name);
      applyTheme(theme);
    }

    connectStream();
    return response;
  }

  async function createThread() {
    await api.createThread({});
    live.set(freshLive());
    return refreshState();
  }

  async function switchThread(threadId) {
    await api.switchThread(threadId);
    live.set(freshLive());
    return refreshState();
  }

  async function deleteThread(threadId) {
    await api.deleteThread(threadId);
    live.set(freshLive());
    return refreshState();
  }

  async function focusAgent(agentId) {
    await api.focusAgent(agentId);
    return refreshState({ keepLive: true });
  }

  async function switchModel(value, variant) {
    const [providerId, modelId] = value.split("::");
    await api.switchModel(providerId, modelId, variant || "");
    return refreshState({ keepLive: true });
  }

  async function switchLeadPersona(leadPersona) {
    await api.switchLeadPersona(leadPersona);
    return refreshState({ keepLive: true });
  }

  async function sendMessage(text) {
    live.update((current) => ({
      ...current,
      pendingUserMessages: [
        ...current.pendingUserMessages,
        { id: `pending-${Date.now()}`, text },
      ],
    }));
    await api.sendMessage(text);
  }

  async function retryLast() {
    await api.retryLast();
    return refreshState({ keepLive: true });
  }

  async function interrupt() {
    await api.interrupt();
  }

  async function undo(body = {}) {
    await api.undo(body);
    return refreshState({ keepLive: true });
  }

  async function compact() {
    await api.compact();
    return refreshState({ keepLive: true });
  }

  async function setPermissionMode(mode) {
    await api.setPermissionMode(mode);
    return refreshState({ keepLive: true });
  }

  async function respondPermission(requestId, response) {
    await api.respondPermission(requestId, response);
    return refreshState({ keepLive: true });
  }

  async function updateConfig(config) {
    await api.updateConfig(config);
    return refreshState({ keepLive: true });
  }

  async function executeWorkflow(workflowId, args = []) {
    await api.executeWorkflow(workflowId, args);
    return refreshState({ keepLive: true });
  }

  async function deleteAccount(providerId, identifier) {
    await api.deleteAccount(providerId, identifier);
    return refreshState({ keepLive: true });
  }

  function selectTheme(name) {
    const available = get(themes);
    const theme = available.find((item) => item.name === name);
    if (!theme) return;
    themeName.set(name);
    localStorage.setItem("firmius-web-theme", name);
    applyTheme(theme);
  }

  return {
    snapshot,
    themes,
    themeName,
    live,
    error,
    refreshState,
    createThread,
    switchThread,
    deleteThread,
    focusAgent,
    switchModel,
    switchLeadPersona,
    sendMessage,
    retryLast,
    interrupt,
    undo,
    compact,
    setPermissionMode,
    respondPermission,
    updateConfig,
    executeWorkflow,
    deleteAccount,
    loadThemes,
    selectTheme,
    disconnectStream,
  };
}

export const appStore = createAppStore();

export const viewState = derived(
  [appStore.snapshot, appStore.live, appStore.themes, appStore.themeName],
  ([$snapshot, $live, $themes, $themeName]) => ({
    snapshot: $snapshot,
    live: $live,
    themes: $themes,
    themeName: $themeName,
  }),
);
