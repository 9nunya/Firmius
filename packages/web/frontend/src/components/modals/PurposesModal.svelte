<script>
  import ModalFrame from "./ModalFrame.svelte";

  export let config = {};
  export let purposes = [];
  export let onSave = null;
  export let onClose = null;

  let draftRoutes = {};
  $: draftRoutes = { ...(config?.purposeRoutes || {}) };
  $: categories = Object.keys(config?.modelRouterCategories || {});
  $: allPurposes = Array.from(new Set([...(purposes || []), ...Object.keys(draftRoutes)])).sort();

  function save() {
    onSave?.({
      ...config,
      purposeRoutes: Object.fromEntries(
        Object.entries(draftRoutes).filter(([, value]) => String(value || "").trim()),
      ),
    });
  }
</script>

<ModalFrame title="Purposes" {onClose}>
  <div class="panel">
    <div class="panel-title">Purpose Routes</div>
    {#each allPurposes as purpose}
      <label class="row">
        <span>{purpose}</span>
        <select bind:value={draftRoutes[purpose]}>
          <option value="">(none)</option>
          {#each categories as category}
            <option value={category}>{category}</option>
          {/each}
        </select>
      </label>
    {/each}
  </div>
  <div class="footer">
    <button on:click={save}>Save Routes</button>
  </div>
</ModalFrame>

<style>
  .panel {
    display: grid;
    gap: 0.55rem;
    padding: 0.8rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 82%, white);
  }
  .panel-title {
    font-size: 0.62rem;
    font-weight: 700;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--muted);
  }
  .row {
    display: grid;
    grid-template-columns: minmax(10rem, 1fr) minmax(12rem, 18rem);
    gap: 0.5rem;
    align-items: center;
  }
  select, button {
    min-height: 2rem;
    padding: 0.45rem 0.55rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 85%, white);
    color: var(--fg);
    font: inherit;
  }
  .footer {
    display: flex;
    justify-content: end;
    margin-top: 0.9rem;
  }
  @media (max-width: 720px) {
    .row {
      grid-template-columns: 1fr;
    }
  }
</style>
