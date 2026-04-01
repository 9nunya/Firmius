<script>
  export let threads = [];
  export let currentThreadId = "";
  export let onNewThread = null;
  export let onSwitchThread = null;
  export let onDeleteThread = null;
  export let visible = true;
  export let mobile = false;
  export let onClose = null;
</script>

<aside class:mobile class:visible class="sidebar">
  <header class="sidebar-head">
    <div>
      <div class="brand-mark">Firmius Web</div>
      <p>Threaded orchestration, replayable history, live state.</p>
    </div>
    <div class="head-actions">
      <button class="action-button" on:click={onNewThread} aria-label="Create new thread">
        <span aria-hidden="true">＋</span>
        <span>New</span>
      </button>
      {#if mobile}
        <button class="action-button" on:click={onClose} aria-label="Close threads panel">
          <span aria-hidden="true">✕</span>
          <span>Close</span>
        </button>
      {/if}
    </div>
  </header>

  <section class="thread-panel">
    <div class="section-head">
      <span>Threads</span>
      <span>{threads.length}</span>
    </div>

    <div class="thread-list">
      {#each threads as thread}
        <article
          class:active={thread.threadId === currentThreadId}
          class="thread-card"
        >
          <div class="thread-row">
            <button class="thread-main" on:click={() => onSwitchThread?.(thread.threadId)}>
              <span class="thread-title">{thread.title || thread.threadId}</span>
            </button>
            <button
              class="delete-button"
              aria-label="Delete thread"
              title="Delete thread"
              on:click={() => onDeleteThread?.(thread.threadId)}
            >
              ×
            </button>
          </div>
          <div class="thread-meta">
            {[thread.cwd || "", thread.permissionMode || ""].filter(Boolean).join("  •  ")}
          </div>
        </article>
      {/each}
    </div>
  </section>
</aside>

<style>
  .sidebar {
    display: flex;
    flex-direction: column;
    gap: 0.82rem;
    min-width: 0;
    height: 100%;
    padding: 0.95rem 0.9rem 0.82rem;
    border-right: 1px solid var(--line-soft);
    background: linear-gradient(180deg, color-mix(in srgb, var(--bg) 97%, white), color-mix(in srgb, var(--panel) 96%, black));
    overflow: hidden;
  }

  .sidebar.mobile {
    position: fixed;
    inset: 0 auto 0 0;
    width: min(90vw, 360px);
    z-index: 40;
    transform: translateX(-100%);
    transition: transform 180ms ease;
    box-shadow: 24px 0 60px rgba(0, 0, 0, 0.32);
  }

  .sidebar.mobile.visible {
    transform: translateX(0);
  }

  .sidebar-head,
  .section-head {
    display: flex;
    align-items: start;
    justify-content: space-between;
    gap: 0.9rem;
  }

  .brand-mark {
    font: 800 0.86rem/1 var(--font-display);
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }

  p,
  .thread-meta {
    margin: 0.38rem 0 0;
    color: var(--muted);
    font-size: 0.69rem;
    line-height: 1.35;
  }

  .head-actions {
    display: flex;
    gap: 0.38rem;
  }

  .action-button,
  .thread-card {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 80%, white);
    color: var(--fg);
    font: inherit;
  }

  .thread-row {
    display: flex;
    justify-content: space-between;
    gap: 0.5rem;
    align-items: start;
  }

  .delete-button {
    border: 0;
    background: transparent;
    color: var(--muted);
    cursor: pointer;
    font-size: 0.9rem;
    line-height: 1;
  }

  .action-button {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    min-height: 2rem;
    padding: 0.48rem 0.65rem;
    font-size: 0.73rem;
  }

  .thread-panel {
    min-height: 0;
    display: grid;
    gap: 0.55rem;
  }

  .section-head {
    font-size: 0.58rem;
    font-weight: 700;
    letter-spacing: 0.17em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .thread-list {
    min-height: 0;
    overflow: auto;
    display: grid;
    gap: 0.42rem;
  }

  .thread-card {
    display: grid;
    gap: 0.24rem;
    padding: 0.66rem 0.72rem;
    text-align: left;
    cursor: pointer;
    border-left: 2px solid transparent;
    transition: transform 160ms ease, border-color 160ms ease, background 160ms ease;
  }

  .thread-main {
    border: 0;
    padding: 0;
    background: transparent;
    color: inherit;
    text-align: left;
    cursor: pointer;
  }

  .thread-card:hover {
    transform: translateX(2px);
  }

  .thread-card.active {
    border-left-color: var(--accent);
    background: color-mix(in srgb, var(--accent) 7%, var(--panel));
  }

  .thread-title {
    font-weight: 700;
    line-height: 1.28;
    font-size: 0.76rem;
  }

  @media (max-width: 1100px) {
    .sidebar:not(.mobile) {
      display: none;
    }
  }
</style>
