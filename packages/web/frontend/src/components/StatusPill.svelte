<script>
  export let tone = "muted";
  export let label = "";
  export let strong = false;
</script>

<span class:strong class={`status-pill ${tone}`}>{label}</span>

<style>
  .status-pill {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
    min-height: 1.2rem;
    padding: 0.08rem 0.4rem;
    border: 1px solid color-mix(in srgb, currentColor 18%, transparent);
    background: color-mix(in srgb, currentColor 6%, transparent);
    color: color-mix(in srgb, currentColor 88%, white);
    font-size: 0.56rem;
    font-weight: 700;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    border-radius: 999px;
  }

  .status-pill::before {
    content: "";
    width: 0.32rem;
    height: 0.32rem;
    border-radius: 999px;
    background: currentColor;
  }

  .status-pill.strong {
    box-shadow: inset 0 0 0 1px color-mix(in srgb, currentColor 12%, transparent);
  }

  .muted { color: var(--muted); }
  .active { color: var(--warn); }
  .success { color: var(--ok); }
  .error { color: var(--error); }
</style>

