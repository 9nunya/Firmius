<script>
  import { renderMarkdown } from "../lib/markdown.js";
  export let text = "";
  $: html = renderMarkdown(text);
</script>

<div class="markdown">{@html html}</div>

<style>
  .markdown :global(p),
  .markdown :global(ul),
  .markdown :global(h1),
  .markdown :global(h2),
  .markdown :global(h3) {
    margin: 0 0 0.62rem;
  }

  .markdown :global(ul) {
    padding-left: 1.15rem;
  }

  .markdown :global(code) {
    font: 0.84em/1.4 var(--font-mono);
    background: color-mix(in srgb, var(--code-bg) 66%, transparent);
    color: var(--code-fg);
    padding: 0.08rem 0.28rem;
  }

  .markdown :global(pre) {
    margin: 0 0 0.62rem;
    padding: 0.68rem 0.78rem;
    overflow: auto;
    background: color-mix(in srgb, var(--code-bg) 82%, transparent);
    border: 1px solid color-mix(in srgb, var(--line-soft) 80%, transparent);
    color: var(--code-fg);
  }

  .markdown :global(pre code) {
    padding: 0;
    background: transparent;
  }

  .markdown :global(a) {
    color: var(--accent);
    text-decoration: none;
  }
</style>
