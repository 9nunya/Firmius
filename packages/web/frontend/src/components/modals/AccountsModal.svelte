<script>
  import ModalFrame from "./ModalFrame.svelte";

  export let providers = [];
  export let onDeleteAccount = null;
  export let onClose = null;
</script>

<ModalFrame title="Accounts" {onClose}>
  <div class="stack">
    {#each providers as provider}
      <section class="panel">
        <div class="head">
          <div class="title">{provider.id}</div>
          <div class="meta">{provider.type}</div>
        </div>
        {#if provider.accounts?.length}
          {#each provider.accounts as account}
            <div class="row">
              <div>
                <div class="name">{account.identifier}</div>
                <div class="meta">{account.rateLimited ? "rate limited" : "ready"}</div>
              </div>
              <button on:click={() => onDeleteAccount?.(provider.id, account.identifier)}>Delete</button>
            </div>
          {/each}
        {:else}
          <div class="empty">No accounts configured.</div>
        {/if}
      </section>
    {/each}
  </div>
</ModalFrame>

<style>
  .stack, .panel { display: grid; gap: 0.6rem; }
  .panel {
    padding: 0.8rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 82%, white);
  }
  .head, .row {
    display: flex;
    justify-content: space-between;
    gap: 1rem;
    align-items: start;
  }
  .title, .name { font-weight: 700; }
  .meta, .empty { color: var(--muted); font-size: 0.74rem; }
  button {
    min-height: 2rem;
    padding: 0.45rem 0.55rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 85%, white);
    color: var(--fg);
    font: inherit;
    cursor: pointer;
  }
</style>
