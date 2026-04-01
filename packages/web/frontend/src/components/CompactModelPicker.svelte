<script>
  export let modelOptions = [];
  export let currentModel = "";
  export let currentVariant = "";
  export let variants = [];
  export let onSwitchModel = null;

  let open = false;
  let query = "";
  let variantValue = "";

  $: variantValue = currentVariant || "";
  $: filtered = modelOptions.filter((option) => {
    const haystack = `${option.label} ${option.value}`.toLowerCase();
    return haystack.includes(query.trim().toLowerCase());
  });
  $: currentLabel =
    modelOptions.find((option) => option.value === currentModel)?.label || "Select model";

  function selectModel(value) {
    onSwitchModel?.(value, variantValue);
    open = false;
    query = "";
  }
</script>

<div class="picker">
  <button class="picker-button" on:click={() => (open = !open)} title={currentLabel}>
    <span class="icon">Model</span>
    <span class="label">{currentLabel}</span>
  </button>

  <select class="variant" value={variantValue} on:change={(event) => onSwitchModel?.(currentModel, event.currentTarget.value)}>
    <option value="">default</option>
    {#each variants as variant}
      <option value={variant.variantName}>{variant.variantName}</option>
    {/each}
  </select>

  {#if open}
    <div class="menu">
      <input bind:value={query} placeholder="Search model…" aria-label="Search models" />
      <div class="results">
        {#each filtered.slice(0, 40) as option}
          <button class:active={option.value === currentModel} on:click={() => selectModel(option.value)}>
            {option.label}
          </button>
        {/each}
      </div>
    </div>
  {/if}
</div>

<style>
  .picker {
    position: relative;
    display: flex;
    gap: 0.35rem;
    align-items: stretch;
    min-width: 0;
  }

  .picker-button,
  .variant,
  .menu input,
  .results button {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 82%, white);
    color: var(--fg);
    font: inherit;
  }

  .picker-button,
  .variant {
    min-width: 0;
    min-height: 2.1rem;
    padding: 0.42rem 0.56rem;
  }

  .picker-button {
    display: flex;
    align-items: center;
    gap: 0.45rem;
    flex: 1 1 auto;
    text-align: left;
    cursor: pointer;
  }

  .label {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    font-size: 0.72rem;
  }

  .icon {
    color: var(--accent);
    font-size: 0.6rem;
    font-weight: 700;
    letter-spacing: 0.1em;
    text-transform: uppercase;
  }

  .variant {
    width: 6rem;
    font-size: 0.7rem;
  }

  .menu {
    position: absolute;
    left: 0;
    bottom: calc(100% + 0.32rem);
    width: min(24rem, calc(100vw - 1rem));
    z-index: 35;
    display: grid;
    gap: 0.42rem;
    padding: 0.45rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 96%, black);
    box-shadow: var(--panel-shadow);
  }

  .menu input {
    padding: 0.46rem 0.54rem;
    font-size: 0.72rem;
  }

  .results {
    max-height: min(14rem, 48vh);
    overflow: auto;
    display: grid;
    gap: 0.28rem;
  }

  .results button {
    padding: 0.42rem 0.5rem;
    text-align: left;
    cursor: pointer;
    font-size: 0.7rem;
  }

  .results button.active {
    border-color: color-mix(in srgb, var(--accent) 30%, var(--line-soft));
    background: color-mix(in srgb, var(--accent) 9%, var(--panel));
  }

  @media (max-width: 820px) {
    .variant {
      width: 5.1rem;
    }

    .menu {
      left: auto;
      right: 0;
      width: min(22rem, calc(100vw - 1rem));
    }
  }
</style>

