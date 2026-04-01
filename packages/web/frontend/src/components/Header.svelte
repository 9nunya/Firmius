<script>
  import StatusPill from "./StatusPill.svelte";

  export let thread = null;
  export let focusedAgent = null;
  export let live = null;
  export let queuedCount = 0;
  export let hasMultipleAgents = false;
  export let onToggleSidebar = null;
  export let onToggleInspector = null;
  export let onOpenRouter = null;
  export let onOpenPurposes = null;
  export let onOpenAccounts = null;
  export let onOpenQuotas = null;
  export let onOpenConnect = null;

  $: connectionLabel =
    live?.streamState === "connected"
      ? "live"
      : live?.streamState === "disconnected"
        ? "reconnecting"
        : live?.streamState === "error"
          ? "error"
          : "connecting";

  $: subline = [
    thread?.cwd || "",
    thread?.leadPersona || "",
    focusedAgent?.friendlyName || focusedAgent?.identityName || "",
  ]
    .filter(Boolean)
    .join("  •  ");
</script>

<header class="chat-header">
  <div class="left">
    <div class="title-row">
      <button class="icon-button" on:click={onToggleSidebar} aria-label="Toggle threads" title="Threads">
        <span aria-hidden="true">☰</span>
      </button>
      <div class="title-copy">
        <div class="eyebrow">Current Thread</div>
        <h1>{thread?.title || "New Thread"}</h1>
      </div>
    </div>
    <p>{subline || "Create a thread or send a message to begin."}</p>
  </div>

  <div class="right">
    <div class="status-row">
      <StatusPill tone={live?.streamState === "connected" ? "success" : live?.streamState === "disconnected" ? "active" : "muted"} label={connectionLabel} />
      {#if queuedCount}
        <StatusPill tone="active" label={`${queuedCount} queued`} />
      {/if}
      {#if hasMultipleAgents}
        <StatusPill tone="muted" label="multi-agent" />
      {/if}
    </div>

    <div class="action-row">
      <button class="text-button" on:click={onOpenRouter}>Router</button>
      <button class="text-button" on:click={onOpenPurposes}>Purposes</button>
      <button class="text-button" on:click={onOpenAccounts}>Accounts</button>
      <button class="text-button" on:click={onOpenQuotas}>Quotas</button>
      <button class="text-button" on:click={onOpenConnect}>Connect</button>
      <button class="icon-button" title="Toggle inspector" on:click={onToggleInspector} aria-label="Toggle inspector">
        <span aria-hidden="true">◫</span>
      </button>
    </div>
  </div>
</header>

<style>
  .chat-header {
    position: sticky;
    top: 0;
    z-index: 20;
    display: flex;
    justify-content: space-between;
    align-items: end;
    gap: 1rem;
    padding: 0.92rem 1.15rem 0.82rem;
    border-bottom: 1px solid var(--line-soft);
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--bg) 98%, white), color-mix(in srgb, var(--panel) 88%, transparent));
    backdrop-filter: blur(12px);
  }

  .left,
  .right {
    display: grid;
    gap: 0.42rem;
  }

  .left {
    min-width: 0;
  }

  .title-row,
  .status-row,
  .action-row {
    display: flex;
    gap: 0.45rem;
    align-items: center;
    flex-wrap: wrap;
  }

  .title-copy {
    min-width: 0;
  }

  h1 {
    margin: 0.14rem 0 0;
    font: 800 clamp(1.05rem, 1.35vw, 1.36rem)/1.02 var(--font-display);
    letter-spacing: -0.025em;
  }

  .eyebrow,
  p {
    margin: 0;
    color: var(--muted);
  }

  .eyebrow {
    font-size: 0.58rem;
    font-weight: 700;
    letter-spacing: 0.18em;
    text-transform: uppercase;
  }

  p {
    max-width: 70ch;
    font-size: 0.74rem;
    line-height: 1.4;
  }

  .icon-button,
  .text-button {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 82%, white);
    color: var(--fg);
    min-height: 2.1rem;
    font: 700 0.88rem/1 var(--font-sans);
    cursor: pointer;
    transition: border-color 150ms ease, background 150ms ease, transform 150ms ease;
  }

  .icon-button {
    width: 2.1rem;
    min-width: 2.1rem;
    padding: 0.48rem 0;
  }

  .text-button {
    padding: 0.48rem 0.62rem;
    font-size: 0.64rem;
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  .icon-button:hover {
    transform: translateY(-1px);
    border-color: color-mix(in srgb, var(--accent) 32%, var(--line-soft));
  }

  @media (max-width: 1100px) {
    .chat-header {
      align-items: start;
      flex-direction: column;
      padding-inline: 0.88rem;
    }

    .right {
      width: 100%;
      display: grid;
      gap: 0.45rem;
    }
  }
</style>
