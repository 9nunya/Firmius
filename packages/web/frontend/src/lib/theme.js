function get(node, path, fallback = "") {
  const keys = path.split(".");
  let current = node;
  for (const key of keys) {
    if (!current || typeof current !== "object" || !(key in current)) {
      return fallback;
    }
    current = current[key];
  }
  return typeof current === "string" ? current : fallback;
}

export function applyTheme(theme) {
  if (!theme) return;
  const root = document.documentElement;
  root.style.setProperty("--bg", get(theme, "base.bg", "#f3efe7"));
  root.style.setProperty("--fg", get(theme, "base.fg", "#1f1d1b"));
  root.style.setProperty("--line", get(theme, "base.border", "#d7cabb"));
  root.style.setProperty("--line-soft", get(theme, "base.separator", "#e2d6c8"));
  root.style.setProperty("--accent", get(theme, "base.highlight", "#1f6b5c"));
  root.style.setProperty("--muted", get(theme, "base.dim", "#6f675e"));
  root.style.setProperty("--panel", get(theme, "input.bg", "rgba(255,251,244,0.92)"));
  root.style.setProperty("--panel-strong", get(theme, "chat.bg", "#fffaf0"));
  root.style.setProperty("--user", get(theme, "chat.user_prefix", "#83a598"));
  root.style.setProperty("--assistant", get(theme, "chat.agent_prefix", "#d3869b"));
  root.style.setProperty("--timestamp", get(theme, "chat.timestamp", "#928374"));
  root.style.setProperty("--code-bg", get(theme, "chat.markdown.code_bg", "#201d1b"));
  root.style.setProperty("--code-fg", get(theme, "chat.markdown.code_fg", "#f7f3ef"));
  root.style.setProperty("--tool-read", get(theme, "tool_blocks.specific.file_read.fg", "#3a6ea5"));
  root.style.setProperty("--tool-edit", get(theme, "tool_blocks.specific.file_edit.fg", "#1f6b5c"));
  root.style.setProperty("--tool-process", get(theme, "tool_blocks.specific.terminal.fg", "#9b5d1a"));
  root.style.setProperty("--tool-subagent", get(theme, "tool_blocks.specific.subagent.fg", "#5f4ea3"));
  root.style.setProperty("--ok", get(theme, "status_bar.streaming.normal.bg", "#2f855a"));
  root.style.setProperty("--warn", get(theme, "status_bar.provider_waiting.normal.bg", "#c18a24"));
  root.style.setProperty("--error", get(theme, "status_bar.error.normal.fg", "#b42318"));
}
