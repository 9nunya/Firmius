<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { phaseLabel, phaseTone, subagentMeta } from "../../lib/tooling.js";

  export let tool;
  export let live = null;
  export let relatedAgent = null;
  export let onFocusAgent = null;
  $: view = subagentMeta(tool, relatedAgent, live);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-subagent)"
  eyebrow="summon_subagent"
  title={view.task || "Summoning subagent"}
  subtitle={[view.persona, view.model].filter(Boolean).join("  •  ")}
  meta={[
    view.subagentId ? `agent ${view.subagentId.slice(0, 8)}` : "",
    view.status,
  ].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
  actionLabel={view.subagentId ? "Focus Agent" : ""}
  onAction={() => onFocusAgent?.(view.subagentId)}
>
  {#if view.liveText}
    <pre>{view.liveText}</pre>
  {:else if view.summary}
    <pre>{view.summary}</pre>
  {:else}
    <div class="placeholder">Subagent output will appear here when available.</div>
  {/if}
  {#if view.fallbackUsed || view.category || view.artifactsCreated || view.artifactsUpdated}
    <div class="facts">
      {#if view.category}<span>{view.category}</span>{/if}
      {#if view.fallbackUsed}<span>fallback</span>{/if}
      {#if view.artifactsCreated}<span>+{view.artifactsCreated} artifact</span>{/if}
      {#if view.artifactsUpdated}<span>~{view.artifactsUpdated} artifact</span>{/if}
    </div>
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
  }
  .placeholder {
    color: var(--muted);
    padding: 0.85rem;
    border: 1px dashed color-mix(in srgb, var(--tool-subagent) 22%, var(--line));
  }
  .facts {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
  }
  .facts span {
    padding: 0.28rem 0.5rem;
    border: 1px solid color-mix(in srgb, var(--tool-subagent) 22%, var(--line));
    color: var(--muted);
    font-size: 0.68rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
</style>
