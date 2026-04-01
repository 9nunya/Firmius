export function parseJson(raw) {
  if (raw == null || raw === "") return null;
  if (typeof raw === "object") return raw;
  try {
    return JSON.parse(raw);
  } catch (_) {
    return null;
  }
}

export function formatJson(raw) {
  const parsed = parseJson(raw);
  if (parsed !== null) {
    return JSON.stringify(parsed, null, 2);
  }
  return String(raw ?? "");
}

export function rawToolText(...values) {
  for (const value of values) {
    if (value == null) continue;
    if (typeof value === "string") {
      if (value.trim()) return value;
      continue;
    }
    if (typeof value === "object") {
      const text = formatJson(value);
      if (text.trim()) return text;
    }
  }
  return "";
}

export function phaseTone(phase = "preparing") {
  if (phase === "finished") return "success";
  if (phase === "error") return "error";
  if (phase === "running" || phase === "called") return "active";
  return "muted";
}

export function phaseLabel(phase = "preparing") {
  return (
    {
      preparing: "preparing",
      called: "queued",
      running: "running",
      finished: "success",
      error: "error",
    }[phase] || phase
  );
}

export function toolFamily(name = "") {
  const normalized = String(name || "").toLowerCase();
  if (normalized.includes("file_edit")) return "file_edit";
  if (normalized.includes("file_read")) return "file_read";
  if (normalized.includes("list_directory")) return "list_directory";
  if (normalized.includes("grep") || normalized.includes("glob")) return "search";
  if (
    normalized.includes("process") ||
    normalized.includes("terminal") ||
    normalized.includes("bash") ||
    normalized.includes("python_execute")
  ) {
    return "process";
  }
  if (normalized.includes("subagent") || normalized.includes("agent")) return "subagent";
  if (normalized.includes("todo") || normalized.includes("plan") || normalized.includes("chunk")) {
    return "work";
  }
  return "generic";
}

export function toolAccent(name = "") {
  const family = toolFamily(name);
  if (family === "file_read") return "var(--tool-read)";
  if (family === "file_edit") return "var(--tool-edit)";
  if (family === "process") return "var(--tool-process)";
  if (family === "subagent") return "var(--tool-subagent)";
  if (family === "work") return "var(--tool-work)";
  return "var(--accent)";
}

function basename(path = "") {
  if (!path) return "";
  const parts = String(path).split("/");
  return parts[parts.length - 1] || path;
}

export function summarizeTool(tool) {
  const name = tool?.name || "tool";
  const args = parseJson(tool?.args);
  const family = toolFamily(name);

  if (family === "file_read") {
    return args?.path ? `read ${basename(args.path)}` : "reading file";
  }
  if (family === "file_edit") {
    if (args?.patch) {
      return `patch ${basename(args.path)}`;
    }
    if (Array.isArray(args?.files) && args.files.length) {
      return `edit ${args.files.length} files`;
    }
    return args?.path ? `edit ${basename(args.path)}` : "editing files";
  }
  if (family === "process") {
    return args?.command ? args.command : name;
  }
  if (family === "search") {
    return args?.pattern ? `${name} "${args.pattern}"` : name;
  }
  if (family === "subagent") {
    return args?.task ? truncate(args.task, 72) : name;
  }
  if (family === "work") {
    return args?.text || args?.title || name;
  }
  return name.replaceAll("_", " ");
}

export function truncate(value, max = 120) {
  const text = String(value ?? "");
  if (text.length <= max) return text;
  return `${text.slice(0, max - 1)}…`;
}

export function previewText(value, maxLines = 12) {
  const text = String(value ?? "");
  const lines = text.split("\n");
  if (lines.length <= maxLines) return text;
  return `${lines.slice(0, maxLines).join("\n")}\n…`;
}

export function fileEditSections(resultText) {
  const parsed = parseJson(resultText);
  if (!parsed || typeof parsed !== "object") return [];
  const files = Array.isArray(parsed.files) ? parsed.files : [parsed];
  return files.map((file) => ({
    kind: "file",
    label: file.path || parsed.path || "edited file",
    mode: file.mode || parsed.mode || "",
    diff: typeof file.diff_preview === "string" ? file.diff_preview : "",
    operations: Array.isArray(file.operations) ? file.operations : [],
    watchState: file.watch_state || parsed.watch_state || "",
    stats: [
      typeof file.applied_edits === "number" ? `${file.applied_edits} edits` : "",
      typeof file.added_lines === "number" ? `+${file.added_lines}` : "",
      typeof file.removed_lines === "number" ? `-${file.removed_lines}` : "",
    ].filter(Boolean),
    raw: file,
  }));
}

export function fileEditFallback(tool) {
  const args = parseJson(tool?.args) || {};
  return rawToolText(
    tool?.result,
    args?.diff,
    args?.content,
    args?.instructions,
    args,
  );
}

function normalizeLines(value) {
  if (Array.isArray(value)) {
    return value.map((line) => String(line ?? ""));
  }
  if (typeof value === "string") {
    return value.split("\n");
  }
  return [];
}

function summarizeOperation(operation = {}) {
  const oldLines = normalizeLines(operation.old_lines);
  const newLines = normalizeLines(operation.new_lines);
  const removed = oldLines.map((line) => ({ type: "-", text: line }));
  const added = newLines.map((line) => ({ type: "+", text: line }));
  return {
    label: operation.description || operation.op || "edit",
    lines: [...removed, ...added],
  };
}

export function fileEditView(tool) {
  const args = parseJson(tool?.args) || {};
  const result = parseJson(tool?.result) || {};
  const resultSections = fileEditSections(tool?.result);
  const argFiles = Array.isArray(args.files) ? args.files : args.path || args.content || args.patch || args.edits ? [args] : [];
  const inferredSections = argFiles.map((file) => {
    const operations = Array.isArray(file.edits) ? file.edits : Array.isArray(file.operations) ? file.operations : [];
    return {
      label: file.path || file.name || "edited file",
      mode: file.mode || result.mode || (file.patch ? "patch" : ""),
      stats: [],
      watchState: result.watch_state || "",
      operations: operations.map(summarizeOperation).filter((entry) => entry.lines.length),
      contentPreview: typeof file.content === "string" ? file.content : (typeof file.patch === "string" ? file.patch : ""),
    };
  });

  const sections = resultSections.length
    ? resultSections.map((section) => ({
        label: section.label,
        mode: section.mode,
        stats: section.stats,
        watchState: section.watchState,
        operations: Array.isArray(section.raw?.operations)
          ? section.raw.operations.map(summarizeOperation).filter((entry) => entry.lines.length)
          : [],
        diff: section.diff,
        contentPreview: "",
      }))
    : inferredSections;

  const artifact = result.artifact || {};
  const artifactContent = typeof args.content === "string" ? args.content : "";
  if (!sections.length && (result.status || artifact.filename || artifactContent)) {
    sections.push({
      label: artifact.filename || args.name || args.path || "edited artifact",
      mode: result.status || result.mode || "",
      stats: [],
      watchState: "",
      operations: [],
      diff: "",
      contentPreview: artifactContent,
    });
  }

  const totalAdded = sections.reduce(
    (sum, section) =>
      sum +
      section.operations.reduce(
        (count, operation) => count + operation.lines.filter((line) => line.type === "+").length,
        0,
      ),
    0,
  );
  const totalRemoved = sections.reduce(
    (sum, section) =>
      sum +
      section.operations.reduce(
        (count, operation) => count + operation.lines.filter((line) => line.type === "-").length,
        0,
      ),
    0,
  );

  return {
    sections,
    totalAdded,
    totalRemoved,
    summary: result.status || result.mode || args.mode || "",
  };
}

export function searchRows(resultText) {
  const parsed = parseJson(resultText);
  if (Array.isArray(parsed?.matches)) {
    return parsed.matches
      .map((entry) => {
        if (typeof entry === "string") return entry;
        if (!entry || typeof entry !== "object") return "";
        const path = entry.path || "";
        const lineNumber = entry.line_number ?? entry.line ?? "";
        const content = entry.content || entry.text || entry.match || "";
        const prefix = path ? `${path}${lineNumber ? `:${lineNumber}` : ""}` : "";
        return [prefix, content].filter(Boolean).join("  ");
      })
      .filter(Boolean);
  }
  if (!Array.isArray(parsed)) return [];
  return parsed
    .map((entry) => {
      if (typeof entry === "string") return entry;
      if (!entry || typeof entry !== "object") return "";
      const path = entry.path || "";
      const lineNumber = entry.line_number ?? entry.line ?? "";
      const content = entry.content || entry.text || entry.match || "";
      const prefix = path ? `${path}${lineNumber ? `:${lineNumber}` : ""}` : "";
      return [prefix, content].filter(Boolean).join("  ");
    })
    .filter(Boolean);
}

export function directoryRows(resultText) {
  const parsed = parseJson(resultText);
  if (Array.isArray(parsed?.entries)) {
    return parsed.entries
      .map((entry) => {
        if (typeof entry === "string") return entry;
        if (!entry || typeof entry !== "object") return "";
        const isDirectory = entry.type === "directory" || entry.is_directory === true;
        const isFile = entry.type === "file" || entry.is_directory === false;
        const type = isDirectory ? "dir" : isFile ? "file" : "";
        return [type ? `[${type}]` : "", entry.path || entry.name || ""].filter(Boolean).join(" ");
      })
      .filter(Boolean);
  }
  if (!Array.isArray(parsed)) return [];
  return parsed
    .map((entry) => {
      if (typeof entry === "string") return entry;
      if (!entry || typeof entry !== "object") return "";
      const isDirectory = entry.type === "directory" || entry.is_directory === true;
      const isFile = entry.type === "file" || entry.is_directory === false;
      const type = isDirectory ? "dir" : isFile ? "file" : "";
      return [type ? `[${type}]` : "", entry.path || entry.name || ""].filter(Boolean).join(" ");
    })
    .filter(Boolean);
}

export function fileReadPayload(resultText) {
  const parsed = parseJson(resultText);
  if (!parsed || typeof parsed !== "object") return null;
  const content = parsed.content || parsed.text || parsed.output || "";
  if (
    !content &&
    !parsed.path &&
    parsed.line_start == null &&
    parsed.line_end == null &&
    parsed.lines_read == null &&
    !parsed.watch_state
  ) {
    return null;
  }
  return {
    path: parsed.path || "",
    content,
    lineRange:
      parsed.start_line && parsed.end_line
        ? `lines ${parsed.start_line}-${parsed.end_line}`
        : parsed.start_line
          ? `from line ${parsed.start_line}`
          : parsed.line_start && parsed.line_end
            ? `lines ${parsed.line_start}-${parsed.line_end}`
            : parsed.line_start
              ? `from line ${parsed.line_start}`
          : "",
    fullyRead: Boolean(parsed.fully_read || parsed.fullyRead),
    linesRead: parsed.lines_read ?? null,
    watchState: parsed.watch_state || "",
    watchScope: parsed.watch_scope || "",
    redirectedTo: parsed.redirected_to || "",
    instruction: parsed.instruction || "",
  };
}

export function processMeta(tool, process) {
  const args = parseJson(tool?.args);
  const result = parseJson(tool?.result);
  const stdout = process?.stdout ?? result?.stdout ?? result?.output ?? "";
  const stderr = process?.stderr ?? result?.stderr ?? "";
  const hasExitCode = result?.exit_code != null || result?.exitCode != null;
  return {
    command: args?.command || args?.cmd || "",
    cwd: args?.cwd || result?.cwd || "",
    exitCode: process?.exitCode ?? result?.exit_code ?? result?.exitCode,
    durationMs: process?.durationMs ?? result?.duration_ms ?? result?.durationMs,
    stdout,
    stderr,
    output: [stdout, stderr].filter(Boolean).join(stdout && stderr ? "\n" : ""),
    finished: Boolean(process?.finished ?? result?.finished ?? hasExitCode),
  };
}

export function toolBodyText(tool, process = null) {
  const family = toolFamily(tool?.name || "");
  if (family === "process") {
    const view = processMeta(tool, process);
    return rawToolText(view.output, tool?.result, tool?.args);
  }
  return rawToolText(tool?.result, tool?.args);
}

export function subagentMeta(tool, relatedAgent, live) {
  const args = parseJson(tool?.args);
  const result = parseJson(tool?.result);
  const subagentId = tool?.subagentId || result?.agent_id || result?.subagent_id || "";
  const liveThinking = subagentId ? live?.thinkingByAgent?.[subagentId] || "" : "";
  const liveText = subagentId ? live?.textByAgent?.[subagentId] || "" : "";
  return {
    subagentId,
    task: args?.task || "",
    persona: args?.persona || args?.persona_name || args?.title || relatedAgent?.persona || "",
    model: relatedAgent?.modelId || args?.model || "",
    status:
      result?.status ||
      (relatedAgent?.isRunning ? "running" : relatedAgent?.isBooting ? "booting" : ""),
    summary: result?.result || result?.text || result?.summary || "",
    liveText: liveText || liveThinking,
    artifactsCreated: Array.isArray(result?.artifacts_created)
      ? result.artifacts_created.length
      : 0,
    artifactsUpdated: Array.isArray(result?.artifacts_updated)
      ? result.artifacts_updated.length
      : 0,
    fallbackUsed: Boolean(result?.fallback_used),
    category: result?.category || "",
    attemptedCategories: Array.isArray(result?.attempted_categories)
      ? result.attempted_categories
      : [],
  };
}

export function workRows(resultText) {
  const parsed = parseJson(resultText);
  const items =
    Array.isArray(parsed?.items)
      ? parsed.items
      : Array.isArray(parsed?.chunks)
        ? parsed.chunks
        : Array.isArray(parsed)
          ? parsed
          : [];
  return items
    .map((item) => {
      if (typeof item === "string") return item;
      if (!item || typeof item !== "object") return "";
      return [
        item.status || "",
        item.text || item.title || item.goal || item.objective || item.id || item.chunk_id || item.plan_id || "",
      ]
        .filter(Boolean)
        .join("  ");
    })
    .filter(Boolean);
}
