<script>
  import ModalFrame from "./ModalFrame.svelte";

  export let config = {};
  export let providers = [];
  export let onSave = null;
  export let onClose = null;

  let draft = structuredClone(config || {});
  let newName = "";
  let newProviderId = "";
  let newModelId = "";
  let newVariantName = "";

  $: draft = structuredClone(config || {});
  $: providerIds = (providers || []).map((provider) => provider.id);

  function addCategory() {
    if (!newName.trim() || !newProviderId.trim() || !newModelId.trim()) return;
    draft.modelRouterCategories = {
      ...(draft.modelRouterCategories || {}),
      [newName.trim()]: {
        providerId: newProviderId.trim(),
        modelId: newModelId.trim(),
        variantName: newVariantName.trim(),
      },
    };
    newName = "";
    newProviderId = "";
    newModelId = "";
    newVariantName = "";
    draft = { ...draft };
  }

  function removeCategory(name) {
    const next = { ...(draft.modelRouterCategories || {}) };
    delete next[name];
    draft.modelRouterCategories = next;
    if (draft.defaultRouteCategory === name) {
      draft.defaultRouteCategory = "";
    }
    draft = { ...draft };
  }

  function save() {
    onSave?.(draft);
  }
</script>

<ModalFrame title="Router" {onClose}>
  <div class="layout">
    <section class="panel">
      <div class="panel-title">Categories</div>
      {#each Object.entries(draft.modelRouterCategories || {}) as [name, route]}
        <div class="row">
          <div class="name">{name}</div>
          <input bind:value={route.providerId} placeholder="provider" />
          <input bind:value={route.modelId} placeholder="model" />
          <input bind:value={route.variantName} placeholder="variant" />
          <button on:click={() => removeCategory(name)}>Delete</button>
        </div>
      {/each}
    </section>

    <section class="panel">
      <div class="panel-title">Defaults</div>
      <label>
        <span>Default route category</span>
        <select bind:value={draft.defaultRouteCategory}>
          <option value="">(none)</option>
          {#each Object.keys(draft.modelRouterCategories || {}) as name}
            <option value={name}>{name}</option>
          {/each}
        </select>
      </label>
      <label class="toggle">
        <input type="checkbox" bind:checked={draft.enableSubagentRouteFallback} />
        <span>Enable subagent route fallback</span>
      </label>
    </section>

    <section class="panel">
      <div class="panel-title">Add Category</div>
      <input bind:value={newName} placeholder="category" />
      <input bind:value={newProviderId} list="provider-ids" placeholder="provider" />
      <datalist id="provider-ids">
        {#each providerIds as providerId}
          <option value={providerId}></option>
        {/each}
      </datalist>
      <input bind:value={newModelId} placeholder="model" />
      <input bind:value={newVariantName} placeholder="variant" />
      <button on:click={addCategory}>Add</button>
    </section>
  </div>
  <div class="footer">
    <button on:click={save}>Save Router</button>
  </div>
</ModalFrame>

<style>
  .layout {
    display: grid;
    gap: 0.8rem;
  }
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
    grid-template-columns: minmax(10rem, 1fr) repeat(3, minmax(0, 1fr)) auto;
    gap: 0.45rem;
    align-items: center;
  }
  .name {
    font-weight: 700;
  }
  label, .toggle {
    display: grid;
    gap: 0.35rem;
  }
  .toggle {
    grid-template-columns: auto 1fr;
    align-items: center;
  }
  .footer {
    display: flex;
    justify-content: end;
    margin-top: 0.9rem;
  }
  input, select, button {
    min-height: 2rem;
    padding: 0.45rem 0.55rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 85%, white);
    color: var(--fg);
    font: inherit;
  }
  button { cursor: pointer; }
  @media (max-width: 720px) {
    .row {
      grid-template-columns: 1fr;
    }
  }
</style>
