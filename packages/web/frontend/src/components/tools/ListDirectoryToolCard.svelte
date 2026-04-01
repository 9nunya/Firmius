<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { directoryRows, parseJson, phaseLabel, phaseTone } from "../../lib/tooling.js";

  export let tool;
  $: args = parseJson(tool?.args);
  $: rows = directoryRows(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-read)"
  eyebrow="list_directory"
  title={args?.path || "Listing directory"}
  subtitle={tool?.id || ""}
  meta={rows.length ? [`${rows.length} entries`] : []}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  {#if rows.length}
    <div class="list">
      {#each rows.slice(0, 40) as row}
        <div class="row">{row}</div>
      {/each}
    </div>
  {:else}
    <div class="row muted">Directory entries will appear here when the result arrives.</div>
  {/if}
</BaseToolCard>

<style>
  .list { display: grid; gap: 0.4rem; }
  .row {
    padding: 0.62rem 0.76rem;
    border-left: 1px solid color-mix(in srgb, var(--tool-read) 30%, var(--line));
    background: color-mix(in srgb, var(--tool-read) 6%, var(--panel));
    font: 0.82rem/1.5 var(--font-mono);
  }
  .row.muted { color: var(--muted); }
</style>
