<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { phaseLabel, phaseTone, workRows } from "../../lib/tooling.js";

  export let tool;
  $: rows = workRows(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-work)"
  eyebrow={tool?.name || "plan"}
  title="Plan state"
  subtitle={tool?.id || ""}
  meta={rows.length ? [`${rows.length} entries`] : []}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  <div class="list">
    {#if rows.length}
      {#each rows as row}
        <div class="row">{row}</div>
      {/each}
    {:else}
      <div class="row muted">Plan details will appear here when available.</div>
    {/if}
  </div>
</BaseToolCard>

<style>
  .list { display: grid; gap: 0.45rem; }
  .row {
    padding: 0.62rem 0.76rem;
    border-left: 1px solid color-mix(in srgb, var(--tool-work) 30%, var(--line));
    background: color-mix(in srgb, var(--tool-work) 6%, var(--panel));
    font: 0.82rem/1.5 var(--font-mono);
  }
  .row.muted { color: var(--muted); }
</style>
