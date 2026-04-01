<script>
  import StatusPill from "../StatusPill.svelte";

  export let tool;
  export let accent = "var(--accent)";
  export let eyebrow = "";
  export let title = "";
  export let subtitle = "";
  export let meta = [];
  export let tone = "muted";
  export let phaseLabel = "";
  export let actionLabel = "";
  export let onAction = null;
</script>

<article class="tool-frame" style={`--tool-accent:${accent}`}>
  <div class="rail"></div>

  <header class="tool-head">
    <div class="identity">
      <div class="eyebrow">{eyebrow || tool?.name || "tool"}</div>
      <h4>{title || tool?.name || "tool"}</h4>
      {#if subtitle}
        <p>{subtitle}</p>
      {/if}
    </div>

    <div class="tool-meta">
      {#if actionLabel}
        <button class="action-button" on:click={onAction}>{actionLabel}</button>
      {/if}
      <StatusPill tone={tone} label={phaseLabel} strong={true} />
    </div>
  </header>

  {#if meta.length}
    <div class="meta-row">
      {#each meta as item}
        <span>{item}</span>
      {/each}
    </div>
  {/if}

  <div class="tool-body">
    <slot></slot>
  </div>
</article>

<style>
  .tool-frame {
    position: relative;
    display: grid;
    width: 100%;
    min-width: 0;
    gap: clamp(0.56rem, 0.45rem + 0.55vw, 0.84rem);
    padding: clamp(0.72rem, 0.62rem + 0.6vw, 0.96rem) clamp(0.74rem, 0.62rem + 0.7vw, 0.98rem) clamp(0.72rem, 0.62rem + 0.6vw, 0.96rem) clamp(0.82rem, 0.7rem + 0.8vw, 1.08rem);
    border: 1px solid var(--line-soft);
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--panel-strong) 90%, white), color-mix(in srgb, var(--panel) 88%, black)),
      linear-gradient(90deg, color-mix(in srgb, var(--tool-accent) 6%, transparent), transparent 30%);
    overflow: hidden;
  }

  .rail {
    position: absolute;
    inset: 0 auto 0 0;
    width: 2px;
    background: linear-gradient(180deg, var(--tool-accent), color-mix(in srgb, var(--tool-accent) 20%, transparent));
  }

  .tool-head,
  .tool-meta,
  .meta-row {
    display: flex;
    align-items: start;
    justify-content: space-between;
    gap: clamp(0.46rem, 0.38rem + 0.45vw, 0.76rem);
    flex-wrap: wrap;
  }

  .tool-head {
    align-items: flex-start;
  }

  .identity {
    display: grid;
    gap: 0.18rem;
    flex: 1 1 14rem;
    min-width: min(100%, 12rem);
  }

  .tool-meta {
    flex: 0 1 auto;
    margin-inline-start: auto;
  }

  .eyebrow {
    font-size: clamp(0.56rem, 0.54rem + 0.08vw, 0.62rem);
    font-weight: 700;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--muted);
  }

  h4 {
    margin: 0.16rem 0 0;
    font-size: clamp(0.82rem, 0.76rem + 0.22vw, 0.92rem);
    line-height: 1.2;
  }

  p {
    margin: 0.26rem 0 0;
    color: var(--muted);
    font-size: clamp(0.68rem, 0.64rem + 0.18vw, 0.74rem);
    line-height: 1.35;
  }

  .meta-row {
    color: var(--muted);
    font-size: clamp(0.63rem, 0.6rem + 0.16vw, 0.69rem);
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  .tool-body {
    display: grid;
    gap: clamp(0.5rem, 0.42rem + 0.46vw, 0.72rem);
    min-width: 0;
  }

  .action-button {
    border: 1px solid color-mix(in srgb, var(--tool-accent) 24%, var(--line-soft));
    background: color-mix(in srgb, var(--tool-accent) 8%, transparent);
    color: var(--fg);
    padding: 0.48rem 0.64rem;
    font: 700 0.68rem/1 var(--font-sans);
    letter-spacing: 0.08em;
    text-transform: uppercase;
    cursor: pointer;
  }
</style>
