<script>
  import CompactModelPicker from "./CompactModelPicker.svelte";
  export let modelOptions = [];
  export let currentModel = "";
  export let currentVariant = "";
  export let variants = [];
  export let note = "";
  export let running = false;
  export let onSend = null;
  export let onInterrupt = null;
  export let onRetry = null;
  export let onUndo = null;
  export let onCompact = null;
  export let onRefresh = null;
  export let onSwitchModel = null;

  let value = "";
  function submit() {
    const text = value.trim();
    if (!text) return;
    onSend?.(text);
    value = "";
  }

  function handleKeydown(event) {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      submit();
    }
  }
</script>

<footer class="composer">
  <div class="control-bar">
    <CompactModelPicker
      {modelOptions}
      {currentModel}
      {currentVariant}
      {variants}
      {onSwitchModel}
    />

    <div class="control-group actions">
      <button class="secondary danger" title="Stop current agent" on:click={onInterrupt}>Stop</button>
      <button class="secondary" title="Retry last turn" on:click={onRetry}>Retry</button>
      <button class="secondary" title="Undo last turn" on:click={() => onUndo?.()}>Undo</button>
      <button class="secondary" title="Compact focused agent" on:click={onCompact}>Compact</button>
      <button class="secondary" title="Refresh thread state" on:click={onRefresh}>Refresh</button>
    </div>
  </div>

  <div class="input-shell">
    <textarea
      bind:value
      on:keydown={handleKeydown}
      rows="3"
      placeholder="Send a message…"
      aria-label="Message"
    ></textarea>
    <div class="input-foot">
      <div class="note">{note || "Enter sends. Shift+Enter inserts a newline. Use /workflow_id to run a workflow."}</div>
      <button class="send" title={running ? "Queue message" : "Send"} on:click={submit}>
        {running ? "Queue message" : "Send"}
      </button>
    </div>
  </div>
</footer>

<style>
  .composer {
    position: sticky;
    bottom: 0;
    z-index: 20;
    display: grid;
    gap: 0.42rem;
    padding: 0.68rem 0.95rem 0.84rem;
    border-top: 1px solid var(--line-soft);
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--bg) 88%, transparent), color-mix(in srgb, var(--bg) 98%, white) 30%),
      linear-gradient(180deg, color-mix(in srgb, var(--panel) 92%, white), color-mix(in srgb, var(--bg) 99%, black));
    backdrop-filter: blur(16px);
  }

  .control-bar,
  .control-group,
  .input-foot {
    display: flex;
    gap: 0.38rem;
    align-items: end;
    justify-content: space-between;
    flex-wrap: wrap;
  }

  .control-bar > :first-child {
    flex: 1 1 auto;
    min-width: 0;
  }

  .note {
    color: var(--muted);
    font-size: 0.56rem;
    letter-spacing: 0.08em;
    text-transform: uppercase;
  }

  textarea,
  button {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 84%, white);
    color: var(--fg);
    font: inherit;
  }

  button {
    cursor: pointer;
    font-weight: 700;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    transition: transform 150ms ease, border-color 150ms ease, background 150ms ease;
  }

  button:hover {
    transform: translateY(-1px);
    border-color: color-mix(in srgb, var(--accent) 28%, var(--line-soft));
  }

  .danger {
    border-color: color-mix(in srgb, var(--error) 24%, var(--line-soft));
    color: var(--error);
  }

  .secondary {
    min-height: 2.05rem;
    padding: 0.42rem 0.72rem;
    font-size: 0.66rem;
  }

  .input-shell {
    display: grid;
    gap: 0.38rem;
    padding: 0.56rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 88%, white);
  }

  textarea {
    width: 100%;
    max-width: 100%;
    min-height: 3.2rem;
    max-height: 24vh;
    resize: vertical;
    padding: 0.72rem 0.8rem 0.68rem;
    font: 0.92rem/1.45 var(--font-sans);
  }

  .send {
    min-width: 7.8rem;
    min-height: 2.25rem;
    padding-inline: 0.88rem;
    font-size: 0.72rem;
    background: linear-gradient(180deg, color-mix(in srgb, var(--accent) 88%, white), var(--accent));
    color: white;
    border-color: color-mix(in srgb, var(--accent) 52%, black);
  }

  @media (max-width: 820px) {
    .composer {
      padding-inline: 0.78rem;
    }

    .control-group,
    .input-foot {
      width: 100%;
    }

    .control-group.actions button,
    .send {
      flex: 1 1 0;
    }
  }
</style>
