<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { parseJson, phaseLabel, phaseTone, workRows } from "../../lib/tooling.js";

  export let tool;
  $: rows = workRows(tool?.result);
  $: patchRows = extractPatchRows(tool?.args);

  function extractPatchRows(rawArgs) {
    const args = parseJson(rawArgs);
    const patch = typeof args?.patch === "string" ? args.patch : "";
    if (!patch) return [];
    return patch
      .split("\n")
      .map((line) => line.trim())
      .filter(Boolean)
      .slice(0, 8);
  }
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-work)"
  eyebrow="todo_write"
  title="Todo list updated"
  subtitle={tool?.id || ""}
  meta={[
    rows.length ? `${rows.length} items` : "",
    !rows.length && patchRows.length ? `${patchRows.length} changes` : "",
  ].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  <div class="list">
    {#if rows.length}
      {#each rows as row}
        <div class="row">{row}</div>
      {/each}
    {:else if patchRows.length}
      {#each patchRows as row}
        <div class="row">{row}</div>
      {/each}
    {:else}
      <div class="row muted">Todo update details will appear here.</div>
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
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .row.muted {
    color: var(--muted);
  }
</style>
