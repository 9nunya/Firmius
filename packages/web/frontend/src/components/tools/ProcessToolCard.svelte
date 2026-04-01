<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { phaseLabel, phaseTone, processMeta, previewText } from "../../lib/tooling.js";

  export let tool;
  export let process = null;
  $: view = processMeta(tool, process);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-process)"
  eyebrow={tool?.name || "process"}
  title={view.command || tool?.name || "process"}
  subtitle={view.cwd || tool?.id || ""}
  meta={[
    view.finished ? "finished" : "live output",
    view.exitCode != null ? `exit ${view.exitCode}` : "",
    view.durationMs != null ? `${Math.round(view.durationMs)}ms` : "",
  ].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  {#if view.stdout}
    <div class="stream-label">stdout</div>
    <pre>{previewText(view.stdout, 40)}</pre>
  {/if}
  {#if view.stderr}
    <div class="stream-label stderr">stderr</div>
    <pre class="stderr-block">{previewText(view.stderr, 20)}</pre>
  {:else}
    {#if !view.stdout}
      <div class="placeholder">Process output will stream here as the command runs.</div>
    {/if}
  {/if}
</BaseToolCard>

<style>
  .stream-label {
    font-size: 0.61rem;
    font-weight: 700;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--muted);
  }

  pre {
    margin: 0;
    padding: 0.85rem 0.95rem;
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--code-bg) 86%, black), color-mix(in srgb, var(--code-bg) 80%, transparent)),
      repeating-linear-gradient(
        180deg,
        transparent,
        transparent 26px,
        color-mix(in srgb, var(--tool-process) 7%, transparent) 26px,
        color-mix(in srgb, var(--tool-process) 7%, transparent) 27px
      );
    color: var(--code-fg);
    font: 0.81rem/1.6 var(--font-mono);
    white-space: pre-wrap;
    overflow: auto;
  }

  .stderr {
    color: color-mix(in srgb, var(--error) 72%, var(--muted));
  }

  .stderr-block {
    border-left: 2px solid color-mix(in srgb, var(--error) 48%, transparent);
  }

  .placeholder {
    color: var(--muted);
    padding: 0.85rem;
    border: 1px dashed color-mix(in srgb, var(--tool-process) 22%, var(--line));
  }
</style>
