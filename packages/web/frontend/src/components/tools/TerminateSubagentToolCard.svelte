<script>
  import BaseToolCard from "./BaseToolCard.svelte";
  import { parseJson, phaseLabel, phaseTone } from "../../lib/tooling.js";

  export let tool;
  $: args = parseJson(tool?.args);
  $: result = parseJson(tool?.result);
</script>

<BaseToolCard
  {tool}
  accent="var(--tool-subagent)"
  eyebrow="terminate_subagent"
  title={args?.agent_id || args?.subagent_id || result?.agent_id || "Terminate subagent"}
  subtitle={result?.message || result?.status || ""}
  meta={[result?.status].filter(Boolean)}
  tone={phaseTone(tool?.phase)}
  phaseLabel={phaseLabel(tool?.phase)}
>
  <div class="detail">{result?.message || result?.status || "Termination details will appear here."}</div>
</BaseToolCard>

<style>
  .detail {
    padding: 0.8rem 0.9rem;
    border: 1px solid color-mix(in srgb, var(--tool-subagent) 22%, var(--line));
    background: color-mix(in srgb, var(--tool-subagent) 6%, var(--panel));
  }
</style>
