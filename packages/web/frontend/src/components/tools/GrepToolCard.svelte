<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { parseJson, phaseLabel, phaseTone, searchRows } from "../../lib/tooling.js";

  export let tool;
  $: args = parseJson(tool?.args);
  $: rows = searchRows(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-read)"
  eyebrow="grep"
  title={args?.pattern ? `grep "${args.pattern}"` : "grep"}
  subtitle={args?.path || tool?.id || ""}
  meta={rows.length ? [`${rows.length} matches`] : []}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  {#if rows.length}
    <div class="list">
      {#each rows.slice(0, 28) as row}
        <div class="row">{row}</div>
      {/each}
    </div>
  {:else}
    <div class="row muted">Matches will appear here when the search result arrives.</div>
  {/if}
</BaseToolCard>

<style>
  .list { display: grid; gap: 0.4rem; }
  .row {
    padding: 0.62rem 0.76rem;
    border-left: 1px solid color-mix(in srgb, var(--tool-read) 30%, var(--line));
    background: color-mix(in srgb, var(--tool-read) 6%, var(--panel));
    font: 0.82rem/1.5 var(--font-mono);
    white-space: pre-wrap;
  }
  .row.muted { color: var(--muted); }
</style>
