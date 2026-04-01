<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { fileEditView, phaseLabel, phaseTone } from "../../lib/tooling.js";

  export let tool;
  $: view = fileEditView(tool);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-edit)"
  eyebrow="file_edit"
  title={view.sections.length > 1 ? `Edited ${view.sections.length} files` : view.sections[0]?.label || "Editing file"}
  subtitle={tool?.id || ""}
  meta={[
    view.sections.length > 1 ? `${view.sections.length} targets` : "",
    view.totalAdded ? `+${view.totalAdded}` : "",
    view.totalRemoved ? `-${view.totalRemoved}` : "",
    view.summary,
  ].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  {#if view.sections.length}
    {#each view.sections as file}
    <section class="panel">
      <div class="head">
        <div class="title">{file.label}</div>
        <div class="meta">{[file.mode, ...file.stats, file.watchState].filter(Boolean).join("  •  ")}</div>
      </div>
      {#if file.diff}
        <pre class="diff">{file.diff}</pre>
      {:else if file.operations.length}
        <div class="ops">
          {#each file.operations as operation}
            <section class="op-group">
              <div class="op-label">{operation.label}</div>
              <div class="op-lines">
                {#each operation.lines as line}
                  <div class={`op-line ${line.type === "+" ? "add" : "remove"}`}>
                    <span class="marker">{line.type}</span>
                    <span>{line.text}</span>
                  </div>
                {/each}
              </div>
            </section>
          {/each}
        </div>
      {:else if file.contentPreview}
        <pre class="diff">{file.contentPreview}</pre>
      {:else}
        <div class="empty">Waiting for edit details.</div>
      {/if}
    </section>
    {/each}
  {:else}
    <div class="empty">Waiting for edit details.</div>
  {/if}
</BaseToolCard>

<style>
  .panel {
    display: grid;
    gap: clamp(0.5rem, 0.45rem + 0.3vw, 0.72rem);
    padding: clamp(0.68rem, 0.62rem + 0.45vw, 0.9rem);
    border: 1px solid color-mix(in srgb, var(--tool-edit) 22%, var(--line));
    background: color-mix(in srgb, var(--tool-edit) 7%, var(--panel));
  }
  .head { display: grid; gap: 0.22rem; }
  .title { font-weight: 700; }
  .meta { color: var(--muted); font-size: 0.78rem; }
  pre {
    margin: 0;
    padding: clamp(0.65rem, 0.58rem + 0.38vw, 0.84rem);
    background: color-mix(in srgb, var(--code-bg) 82%, transparent);
    color: var(--code-fg);
    font: 0.81rem/1.55 var(--font-mono);
    white-space: pre-wrap;
    overflow: auto;
  }
  .diff { border-left: 2px solid color-mix(in srgb, var(--tool-edit) 55%, transparent); }
  .ops { display: grid; gap: 0.45rem; }
  .op-group {
    display: grid;
    gap: 0.35rem;
  }
  .op-label {
    color: var(--muted);
    font-size: 0.72rem;
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  .op-lines {
    display: grid;
    gap: 1px;
    border: 1px solid color-mix(in srgb, var(--line) 32%, transparent);
    background: color-mix(in srgb, var(--line) 20%, transparent);
  }
  .op-line {
    display: grid;
    grid-template-columns: auto minmax(0, 1fr);
    gap: 0.7rem;
    padding: 0.5rem 0.65rem;
    background: color-mix(in srgb, var(--code-bg) 82%, transparent);
    font: 0.8rem/1.5 var(--font-mono);
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }
  .op-line.add {
    background: color-mix(in srgb, var(--tool-edit) 10%, var(--code-bg));
  }
  .op-line.remove {
    background: color-mix(in srgb, var(--error) 9%, var(--code-bg));
  }
  .marker {
    color: var(--muted);
  }
  .empty {
    color: var(--muted);
    padding: 0.8rem;
    border: 1px dashed color-mix(in srgb, var(--tool-edit) 22%, var(--line));
  }
</style>
