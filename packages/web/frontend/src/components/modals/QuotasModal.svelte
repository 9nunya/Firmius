<script>
  import ModalFrame from "./ModalFrame.svelte";

  export let providers = [];
  export let onClose = null;
</script>

<ModalFrame title="Quotas" {onClose}>
  <div class="stack">
    {#each providers as provider}
      <section class="panel">
        <div class="title">{provider.id}</div>
        {#if Object.keys(provider.quotas || {}).length}
          {#each Object.entries(provider.quotas || {}) as [accountId, buckets]}
            <div class="quota-group">
              <div class="account">{accountId}</div>
              {#each buckets as bucket}
                <div class="row">
                  <span>{bucket.name}</span>
                  <span>{Math.round((bucket.remainingFraction || 0) * 100)}%</span>
                  <span>{bucket.resetTime || "no reset"}</span>
                </div>
                {#if bucket.note}
                  <div class="note">{bucket.note}</div>
                {/if}
              {/each}
            </div>
          {/each}
        {:else}
          <div class="empty">No quota data available.</div>
        {/if}
      </section>
    {/each}
  </div>
</ModalFrame>

<style>
  .stack, .panel, .quota-group { display: grid; gap: 0.6rem; }
  .panel {
    padding: 0.8rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 82%, white);
  }
  .title, .account { font-weight: 700; }
  .row {
    display: grid;
    grid-template-columns: minmax(8rem, 1fr) auto auto;
    gap: 0.8rem;
    color: var(--muted);
    font-size: 0.78rem;
  }
  .note { color: var(--muted); font-size: 0.74rem; }
  .empty { color: var(--muted); font-size: 0.74rem; }
  @media (max-width: 720px) {
    .row { grid-template-columns: 1fr; }
  }
</style>
