<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { fileReadPayload, phaseLabel, phaseTone, previewText } from "../../lib/tooling.js";

  export let tool;
  $: payload = fileReadPayload(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-read)"
  eyebrow="file_read"
  title={payload?.path || "Reading file"}
  subtitle={tool?.id || ""}
  meta={[
    payload?.lineRange,
    payload?.linesRead != null ? `${payload.linesRead} lines` : "",
    payload?.watchState,
    payload?.watchScope,
    payload?.fullyRead ? "fully read" : "",
  ].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  {#if payload?.content}
    <pre>{previewText(payload.content, 28)}</pre>
  {:else if payload?.redirectedTo || payload?.instruction}
    <div class="notice">
      {#if payload.redirectedTo}
        <div>Saved to {payload.redirectedTo}</div>
      {/if}
      {#if payload.instruction}
        <div>{payload.instruction}</div>
      {/if}
    </div>
  {:else}
    <div class="notice">Read metadata is available. Full file content was not returned for this call.</div>
  {/if}
</BaseToolCard>

<style>
  pre {
    margin: 0;
    padding: 0.8rem 0.9rem;
    background: color-mix(in srgb, var(--code-bg) 82%, transparent);
    color: var(--code-fg);
    font: 0.81rem/1.55 var(--font-mono);
    white-space: pre-wrap;
    overflow: auto;
  }
  .notice {
    color: var(--muted);
    padding: 0.8rem 0.9rem;
    border: 1px dashed color-mix(in srgb, var(--tool-read) 22%, var(--line));
    background: color-mix(in srgb, var(--tool-read) 5%, var(--panel));
  }
</style>
