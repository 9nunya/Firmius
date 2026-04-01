<script>
  export let tools = [];

  function parseJson(raw) {
    if (!raw || typeof raw !== "string") return raw || {};
    try {
      return JSON.parse(raw);
    } catch (_) {
      return {};
    }
  }

  function shortenPath(path = "") {
    if (!path) return "";
    const text = String(path);
    const repoMarker = "/Projects/Firmius/";
    const markerIndex = text.indexOf(repoMarker);
    if (markerIndex >= 0) return text.slice(markerIndex + repoMarker.length);
    const segments = text.split("/").filter(Boolean);
    return segments.length <= 3 ? text : segments.slice(-3).join("/");
  }

  function readLabel(tool) {
    const args = parseJson(tool?.args);
    const result = parseJson(tool?.result);
    const path = shortenPath(args.path || result.path || "");
    const lineStart = result.line_start ?? result.start_line;
    const lineEnd = result.line_end ?? result.end_line;
    if (lineStart && lineEnd) return `${path}:${lineStart}-${lineEnd}`;
    if (lineStart) return `${path}:${lineStart}`;
    return path;
  }

  function summarizeBurst(items) {
    const lines = [];
    const reads = items.filter((tool) => tool?.name === "file_read").map(readLabel).filter(Boolean);
    if (reads.length) lines.push(`Read ${reads.join(", ")}`);

    for (const tool of items) {
      const args = parseJson(tool?.args);
      if (tool?.name === "list_directory") {
        lines.push(`Listed ${shortenPath(args.path || "")}`);
      } else if (tool?.name === "grep" || tool?.name === "glob") {
        lines.push(`Searched ${args.pattern || tool.name}`);
      }
    }

    return lines;
  }

  $: lines = summarizeBurst(tools);
</script>

{#if lines.length}
  <div class="burst-inline" aria-label="Search and file read activity">
    {#each lines as line}
      <div class="line">{line}</div>
    {/each}
  </div>
{/if}

<style>
  .burst-inline {
    display: grid;
    gap: 0.16rem;
    padding: 0.02rem 0 0.08rem;
  }

  .line {
    padding-left: 0.2rem;
    color: color-mix(in srgb, var(--tool-read) 64%, white);
    font: 0.77rem/1.35 var(--font-mono);
    white-space: pre-wrap;
    word-break: break-word;
  }
</style>

