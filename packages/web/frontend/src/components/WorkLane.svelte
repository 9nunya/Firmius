<script>
  export let title = "Work";
  export let rows = [];
</script>

{#if rows.length}
  <article class="work-lane">
    <div class="lane-head">{title}</div>
    <div class="lane-list">
      {#each rows as row}
        <div class="lane-row">{row}</div>
      {/each}
    </div>
  </article>
{/if}

<style>
  .work-lane {
    display: grid;
    gap: 0.48rem;
    padding: 0.6rem 0.72rem;
    border: 1px solid color-mix(in srgb, var(--tool-work) 15%, var(--line-soft));
    background: color-mix(in srgb, var(--tool-work) 4%, var(--panel));
  }

  .lane-head {
    color: var(--muted);
    font-size: 0.58rem;
    font-weight: 700;
    letter-spacing: 0.16em;
    text-transform: uppercase;
  }

  .lane-list {
    display: grid;
    gap: 0.32rem;
  }

  .lane-row {
    padding-left: 0.56rem;
    border-left: 2px solid color-mix(in srgb, var(--tool-work) 28%, var(--line-soft));
    font: 0.76rem/1.4 var(--font-mono);
    white-space: pre-wrap;
  }
</style>

