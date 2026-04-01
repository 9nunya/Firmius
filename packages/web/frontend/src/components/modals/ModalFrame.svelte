<script>
  export let title = "";
  export let onClose = null;
</script>

<div
  class="backdrop"
  role="presentation"
  tabindex="-1"
  on:click={(event) => event.target === event.currentTarget && onClose?.()}
  on:keydown={(event) => event.key === "Escape" && onClose?.()}
>
  <div class="modal" role="dialog" aria-modal="true" aria-label={title}>
    <header class="head">
      <h2>{title}</h2>
      <button class="close" on:click={onClose} aria-label="Close modal">Close</button>
    </header>
    <div class="body">
      <slot></slot>
    </div>
  </div>
</div>

<style>
  .backdrop {
    position: fixed;
    inset: 0;
    z-index: 60;
    display: grid;
    place-items: center;
    padding: 1rem;
    background: rgba(5, 8, 10, 0.68);
    backdrop-filter: blur(6px);
  }

  .modal {
    width: min(96vw, 980px);
    max-height: min(88vh, 900px);
    display: grid;
    grid-template-rows: auto minmax(0, 1fr);
    border: 1px solid var(--line-soft);
    background: linear-gradient(180deg, color-mix(in srgb, var(--panel-strong) 96%, white), color-mix(in srgb, var(--bg) 98%, black));
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.4);
  }

  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 1rem;
    padding: 0.9rem 1rem;
    border-bottom: 1px solid var(--line-soft);
  }

  h2 {
    margin: 0;
    font: 800 0.98rem/1.1 var(--font-display);
    letter-spacing: 0.02em;
  }

  .close {
    min-height: 2rem;
    padding: 0.45rem 0.7rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 82%, white);
    color: var(--fg);
    font: 700 0.68rem/1 var(--font-sans);
    letter-spacing: 0.08em;
    text-transform: uppercase;
    cursor: pointer;
  }

  .body {
    min-height: 0;
    overflow: auto;
    padding: 1rem;
  }
  @media (max-width: 720px) {
    .backdrop {
      padding: 0.35rem;
    }
    .modal {
      width: 100%;
      max-height: 96vh;
    }
    .body {
      padding: 0.75rem;
    }
  }
</style>
