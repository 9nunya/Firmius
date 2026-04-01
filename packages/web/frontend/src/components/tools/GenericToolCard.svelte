<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { formatJson, parseJson, phaseLabel, phaseTone, summarizeTool, toolAccent } from "../../lib/tooling.js";

  export let tool;
  $: args = parseJson(tool?.args);
  $: result = parseJson(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent={toolAccent(tool?.name)}
  eyebrow={tool?.name || "tool"}
  title={summarizeTool(tool)}
  subtitle={tool?.id || ""}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  <pre>{formatJson(result ?? args ?? tool?.result ?? tool?.args)}</pre>
</BaseToolCard>

<style>
  pre {
    margin: 0;
    padding: 0.8rem 0.9rem;
    background: color-mix(in srgb, var(--code-bg) 82%, transparent);
    color: var(--code-fg);
    border: 1px solid color-mix(in srgb, var(--line) 38%, transparent);
    font: 0.81rem/1.55 var(--font-mono);
    white-space: pre-wrap;
    overflow: auto;
  }
</style>
