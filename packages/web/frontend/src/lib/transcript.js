import {
  directoryRows,
  fileReadPayload,
  parseJson,
  searchRows,
  toolFamily,
} from "./tooling.js";

function partType(part) {
  return String(part?.type || "");
}

function isToolCallPart(part) {
  const type = partType(part);
  return type === "toolCall" || type === "tool_call";
}

function isToolResultPart(part) {
  const type = partType(part);
  return type === "toolResult" || type === "tool_result";
}

function isTextualPart(part) {
  const type = partType(part);
  return (
    type === "text" ||
    type === "thinking" ||
    type === "notice" ||
    type === "error" ||
    type === "image"
  );
}

function normalizeMessageRole(role) {
  if (typeof role === "string") return role.toLowerCase();
  return (
    {
      0: "system",
      1: "user",
      2: "assistant",
      3: "tool",
      4: "error",
    }[role] || "message"
  );
}

function isVisibleMessage(message) {
  const role = normalizeMessageRole(message?.role);
  const visibility = message?.visibility;
  if (role === "tool" || role === "system") return false;
  if (visibility == null) return true;
  if (typeof visibility === "string") {
    return visibility === "Visible";
  }
  return visibility === 0;
}

function messageKey(message, turnIndex, messageIndex) {
  return message?.id || message?.messageId || `${turnIndex}:${messageIndex}`;
}

function shorten(value, limit = 52) {
  const text = String(value || "");
  if (text.length <= limit) return text;
  return `${text.slice(0, Math.max(0, limit - 1))}…`;
}

function lineRangeSuffix(args) {
  const start = Number.isInteger(args?.start_line) ? args.start_line : -1;
  const end = Number.isInteger(args?.end_line) ? args.end_line : -1;
  return start >= 0 && end >= 0 ? `:${start}-${end}` : "";
}

function quickToolDescriptor(tool) {
  const family = toolFamily(tool?.name || "");
  const args = parseJson(tool?.args) || {};
  if (family === "file_read") {
    return {
      category: "Read",
      target: shorten(`${args.path || ""}${lineRangeSuffix(args)}`.trim()),
    };
  }
  if (family === "list_directory") {
    return {
      category: "List",
      target: shorten(args.path || "."),
    };
  }
  if (family === "search") {
    const pattern = args.pattern || args.query || "…";
    const scope = args.path ? ` in ${shorten(args.path)}` : "";
    return {
      category: "Search",
      target: `"${shorten(pattern, 30)}"${scope}`,
    };
  }
  return { category: "", target: "" };
}

function isQuickTool(tool) {
  const category = quickToolDescriptor(tool).category;
  return category === "Read" || category === "List" || category === "Search";
}

function quickGroupLabel(tools) {
  if (!tools.length) return "";
  const descriptor = quickToolDescriptor(tools[0]);
  const targets = [];
  const seen = new Set();
  let hasError = false;
  for (const tool of tools) {
    const next = quickToolDescriptor(tool);
    if (next.target && !seen.has(next.target)) {
      seen.add(next.target);
      targets.push(next.target);
    }
    if (tool.phase === "error") hasError = true;
  }
  const prefix =
    descriptor.category === "Read"
      ? hasError
        ? "Failed Reading "
        : "Read "
      : descriptor.category === "List"
        ? hasError
          ? "Failed Listing "
          : "Listed "
        : hasError
          ? "Failed Search "
          : "Search ";
  return `${prefix}${targets.join(", ")}`;
}

function cloneMessage(message, parts, turn) {
  return {
    ...message,
    role: normalizeMessageRole(message?.role),
    parts,
    content: parts,
    timestamp: message?.timestamp || turn?.timestamp || 0,
  };
}

function normalizeEpisode(part, toolCallsById) {
  if (isToolCallPart(part)) {
    const id = part.id || part.toolCallId || "";
    if (!id) return null;
    const tool = {
      id,
      toolCallId: id,
      name: part.name || part.toolName || "",
      args: part.args || part.toolArgs || "",
      result: "",
      success: false,
      phase: "called",
      processId: "",
      subagentId: "",
      startedAt: 0,
    };
    toolCallsById.set(id, tool);
    return tool;
  }

  if (isToolResultPart(part)) {
    const id = part.toolCallId || part.id || "";
    if (!id) return null;
    const prior = toolCallsById.get(id) || {
      id,
      toolCallId: id,
      name: part.name || part.toolName || "",
      args: part.args || part.toolArgs || "",
      startedAt: 0,
    };
    const tool = {
      ...prior,
      id,
      toolCallId: id,
      name: part.name || part.toolName || prior.name || "",
      args: part.args || part.toolArgs || prior.args || "",
      result: part.result || "",
      success: Boolean(part.success),
      phase: part.success ? "finished" : "error",
      processId: part.processId || prior.processId || "",
      subagentId:
        part.subagentId ||
        part.agentId ||
        part.subagent_id ||
        part.agent_id ||
        prior.subagentId ||
        "",
    };
    toolCallsById.set(id, tool);
    return tool;
  }

  return null;
}

export function messageParts(message) {
  if (!message || typeof message !== "object") return [];
  if (Array.isArray(message.content)) return message.content;
  if (Array.isArray(message.parts)) return message.parts;
  return [];
}

export function visibleMessages(turns = []) {
  const messages = [];
  for (let turnIndex = 0; turnIndex < turns.length; turnIndex += 1) {
    const turn = turns[turnIndex] || {};
    for (let messageIndex = 0; messageIndex < (turn.messages || []).length; messageIndex += 1) {
      const message = turn.messages[messageIndex];
      if (!isVisibleMessage(message)) continue;
      const parts = messageParts(message).filter(isTextualPart);
      if (!parts.length) continue;
      messages.push({
        ...cloneMessage(message, parts, turn),
        id: messageKey(message, turnIndex, messageIndex),
      });
    }
  }
  return messages;
}

export function historyEntries(turns = []) {
  const entries = [];
  const toolCallsById = new Map();
  const completedToolIds = new Set();
  let quickTools = [];

  for (const turn of turns || []) {
    for (const message of turn.messages || []) {
      for (const part of messageParts(message)) {
        if (isToolResultPart(part)) {
          const id = part.toolCallId || part.id || "";
          if (id) {
            completedToolIds.add(id);
          }
        }
      }
    }
  }

  function flushQuickTools() {
    if (!quickTools.length) return;
    entries.push({
      type: "quickTools",
      label: quickGroupLabel(quickTools),
      tools: quickTools,
    });
    quickTools = [];
  }

  for (let turnIndex = 0; turnIndex < turns.length; turnIndex += 1) {
    const turn = turns[turnIndex] || {};
    for (let messageIndex = 0; messageIndex < (turn.messages || []).length; messageIndex += 1) {
      const message = turn.messages[messageIndex];
      const visible = isVisibleMessage(message);

      const proseParts = [];
      for (const part of messageParts(message)) {
        if (isTextualPart(part)) {
          if (!visible) {
            continue;
          }
          flushQuickTools();
          proseParts.push(part);
          continue;
        }

        const tool = normalizeEpisode(part, toolCallsById);
        if (!tool || !tool.name) {
          continue;
        }

        if (isToolCallPart(part) && completedToolIds.has(tool.id)) {
          continue;
        }

        if (proseParts.length) {
          entries.push({
            type: "message",
            message: {
              ...cloneMessage(message, proseParts.splice(0), turn),
              id: messageKey(message, turnIndex, messageIndex),
            },
          });
        }

        if (isQuickTool(tool)) {
          if (quickTools.length && quickToolDescriptor(quickTools[0]).category !== quickToolDescriptor(tool).category) {
            flushQuickTools();
          }
          quickTools.push(tool);
          continue;
        }

        flushQuickTools();
        entries.push({ type: "tool", tool });
      }

      if (visible && proseParts.length) {
        flushQuickTools();
        entries.push({
          type: "message",
          message: {
            ...cloneMessage(message, proseParts, turn),
            id: messageKey(message, turnIndex, messageIndex),
          },
        });
      }
    }
  }

  flushQuickTools();
  return entries;
}

export function liveWorkRows(tools = {}) {
  return Object.values(tools)
    .filter((tool) => toolFamily(tool?.name || "") === "work")
    .sort((a, b) => (a.startedAt || 0) - (b.startedAt || 0))
    .map((tool) => tool.name || "")
    .filter(Boolean);
}

export function liveSearchBurstTools(tools = {}, persistedToolIds = new Set()) {
  return Object.values(tools)
    .filter((tool) => !persistedToolIds.has(tool.id))
    .filter(isQuickTool)
    .sort((a, b) => (a.startedAt || 0) - (b.startedAt || 0));
}

export function quickToolDetails(tool) {
  const family = toolFamily(tool?.name || "");
  if (family === "file_read") {
    const payload = fileReadPayload(tool?.result);
    if (!payload) return [];
    return [
      payload.path || "",
      payload.lineRange || "",
      payload.content ? shorten(payload.content.replace(/\s+/g, " "), 96) : "",
    ].filter(Boolean);
  }
  if (family === "list_directory") {
    const rows = directoryRows(tool?.result);
    const preview = rows.slice(0, 3);
    return rows.length > 3 ? [...preview, `+${rows.length - 3} more`] : preview;
  }
  if (family === "search") {
    const rows = searchRows(tool?.result);
    const preview = rows.slice(0, 3);
    return rows.length > 3 ? [...preview, `+${rows.length - 3} more`] : preview;
  }
  return [];
}

export function roleName(role) {
  return normalizeMessageRole(role);
}

export function formatTimestamp(timestamp) {
  if (!timestamp) return "";
  try {
    return new Date(Number(timestamp)).toLocaleTimeString([], {
      hour: "numeric",
      minute: "2-digit",
    });
  } catch {
    return "";
  }
}

export function displayAgentName(agent) {
  if (!agent) return "";
  return agent.friendlyName || agent.identityName || agent.title || agent.agentId || "";
}

export function agentStatus(agent, live) {
  if (!agent) return "idle";
  if (live?.providerWaiting?.[agent.agentId]) return "provider waiting";
  if (live?.retryByAgent?.[agent.agentId]) return "retrying";
  if (live?.thinkingByAgent?.[agent.agentId] || live?.textByAgent?.[agent.agentId]) {
    return "streaming";
  }
  if (agent.isRunning) return "running";
  if (agent.isBooting) return "booting";
  return "idle";
}
