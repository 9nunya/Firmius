<script>
  import ToolRenderer from "./tools/ToolRenderer.svelte";
  import MarkdownBlock from "./MarkdownBlock.svelte";
  import { formatTimestamp, messageParts, roleName } from "../lib/transcript.js";

  export let message;
  export let live = null;
  export let agentLookup = {};
  export let onFocusAgent = null;

  $: role = roleName(message?.role);
  $: parts = messageParts(message);
</script>

<article class={`message-card ${role} ${message?.pending ? "pending" : ""}`}>
  <header class="message-head">
    <div class="identity">
      <div class="role-label">{role}</div>
      <div class="timestamp">{formatTimestamp(message?.timestamp)}</div>
    </div>
  </header>

  <div class="message-body">
    {#each parts as part}
      {#if part.type === "text"}
        <div class="chunk prose"><MarkdownBlock text={part.text} /></div>
      {:else if part.type === "thinking"}
        <div class="chunk thinking"><MarkdownBlock text={part.thinking} /></div>
      {:else if part.type === "toolCall" || part.type === "toolResult"}
        <ToolRenderer
          tool={{
            id: part.id || part.toolCallId,
            name: part.name || part.toolName || "tool",
            args: part.args || part.toolArgs || "",
            result: part.result || part.output || "",
            phase: part.type === "toolResult" ? (part.success ? "finished" : "error") : "called",
            subagentId: part.subagentId || "",
          }}
          live={live}
          relatedAgent={part.subagentId ? agentLookup?.[part.subagentId] : null}
          onFocusAgent={onFocusAgent}
        />
      {:else if part.type === "error"}
        <div class="chunk error">{part.errorName}: {part.description}</div>
      {:else if part.type === "notice"}
        <div class="chunk notice">
          <strong>{part.title}</strong>
          <span>{part.message}</span>
        </div>
      {/if}
    {/each}
  </div>
</article>

<style>
  .message-card {
    display: grid;
    gap: clamp(0.34rem, 0.28rem + 0.25vw, 0.44rem);
    padding: 0.06rem 0;
    background: transparent;
    animation: message-rise 180ms ease;
    min-width: 0;
  }

  .message-card.user {
    padding: clamp(0.58rem, 0.48rem + 0.45vw, 0.74rem) clamp(0.64rem, 0.52rem + 0.55vw, 0.84rem);
    border-left: 2px solid color-mix(in srgb, var(--user) 64%, white);
    background: linear-gradient(180deg, color-mix(in srgb, var(--panel) 84%, white), color-mix(in srgb, var(--panel) 80%, black));
  }

  .message-card.user.pending {
    opacity: 0.76;
  }

  .message-head {
    display: flex;
    align-items: center;
    gap: 0.72rem;
  }

  .identity {
    display: flex;
    align-items: baseline;
    gap: 0.44rem;
    flex-wrap: wrap;
  }

  .role-label {
    font-size: 0.57rem;
    font-weight: 700;
    letter-spacing: 0.18em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .timestamp {
    color: var(--timestamp);
    font-size: 0.64rem;
  }

  .message-body {
    display: grid;
    gap: clamp(0.42rem, 0.32rem + 0.45vw, 0.6rem);
    min-width: 0;
  }

  .chunk {
    line-height: 1.62;
    font-size: 0.86rem;
    min-width: 0;
  }

  .chunk.prose {
    max-width: 88ch;
  }

  .thinking {
    color: color-mix(in srgb, var(--muted) 82%, white);
    font-style: italic;
  }

  .error {
    color: var(--error);
  }

  .notice {
    display: grid;
    gap: 0.35rem;
    padding: 0.68rem 0.76rem;
    background: color-mix(in srgb, var(--accent) 6%, transparent);
    border: 1px solid color-mix(in srgb, var(--accent) 10%, var(--line-soft));
  }

  @keyframes message-rise {
    from {
      transform: translateY(10px);
      opacity: 0;
    }
    to {
      transform: translateY(0);
      opacity: 1;
    }
  }
</style>
