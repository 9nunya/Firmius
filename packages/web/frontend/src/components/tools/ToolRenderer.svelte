<script>
  import ChunkToolCard from "./ChunkToolCard.svelte";
  import FileEditToolCard from "./FileEditToolCard.svelte";
  import FileReadToolCard from "./FileReadToolCard.svelte";
  import GenericToolCard from "./GenericToolCard.svelte";
  import GlobToolCard from "./GlobToolCard.svelte";
  import GrepToolCard from "./GrepToolCard.svelte";
  import ListDirectoryToolCard from "./ListDirectoryToolCard.svelte";
  import PlanToolCard from "./PlanToolCard.svelte";
  import ProcessToolCard from "./ProcessToolCard.svelte";
  import SubagentWaitToolCard from "./SubagentWaitToolCard.svelte";
  import SummonSubagentToolCard from "./SummonSubagentToolCard.svelte";
  import TerminateSubagentToolCard from "./TerminateSubagentToolCard.svelte";
  import TodoWriteToolCard from "./TodoWriteToolCard.svelte";

  export let tool;
  export let process = null;
  export let live = null;
  export let relatedAgent = null;
  export let onFocusAgent = null;

  function nameOf(tool) {
    return String(tool?.name || "").toLowerCase();
  }

  function isProcessTool(name) {
    return (
      name.includes("process_") ||
      name.includes("terminal") ||
      name.includes("bash") ||
      name === "python_execute"
    );
  }

  $: toolName = nameOf(tool);
</script>

{#if toolName === "file_edit"}
  <FileEditToolCard {tool} />
{:else if toolName === "file_read"}
  <FileReadToolCard {tool} />
{:else if toolName === "list_directory"}
  <ListDirectoryToolCard {tool} />
{:else if toolName === "grep"}
  <GrepToolCard {tool} />
{:else if toolName === "glob"}
  <GlobToolCard {tool} />
{:else if toolName === "summon_subagent"}
  <SummonSubagentToolCard {tool} {live} {relatedAgent} {onFocusAgent} />
{:else if toolName === "subagent_wait"}
  <SubagentWaitToolCard {tool} {live} {relatedAgent} {onFocusAgent} />
{:else if toolName === "terminate_subagent" || toolName === "subagent_terminate"}
  <TerminateSubagentToolCard {tool} />
{:else if toolName === "todo_write"}
  <TodoWriteToolCard {tool} />
{:else if toolName.startsWith("plan_")}
  <PlanToolCard {tool} />
{:else if toolName.startsWith("chunk_")}
  <ChunkToolCard {tool} />
{:else if isProcessTool(toolName)}
  <ProcessToolCard {tool} {process} />
{:else}
  <GenericToolCard {tool} />
{/if}
