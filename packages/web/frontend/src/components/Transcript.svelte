<script>
  import { tick } from "svelte";
  import MessageCard from "./MessageCard.svelte";
  import ToolRenderer from "./tools/ToolRenderer.svelte";
  import WorkLane from "./WorkLane.svelte";
  import {
    displayAgentName,
    historyEntries,
    liveSearchBurstTools,
    liveWorkRows,
    quickToolDetails,
  } from "../lib/transcript.js";
  import { toolFamily } from "../lib/tooling.js";

  export let history = null;
  export let live = null;
  export let agents = [];
  export let focusedAgentId = "";
  export let onFocusAgent = null;

  let scroller;
  let stickToBottom = true;
  let previousSignature = "";

  $: entries = historyEntries(history?.turns || []);
  $: persistedTools = entries.filter((entry) => entry.type === "tool").map((entry) => entry.tool);
  $: messages = entries.filter((entry) => entry.type === "message").map((entry) => entry.message);
  $: persistedToolIds = new Set(
    persistedTools.map((tool) => tool.id),
  );
  $: liveTools = Object.values(live?.tools || {}).sort(
    (a, b) => (a.startedAt || 0) - (b.startedAt || 0),
  ).filter((tool) => !persistedToolIds.has(tool.id));
  $: liveSearchTools = liveSearchBurstTools(live?.tools || {}, persistedToolIds);
  $: liveDetailedTools = liveTools.filter((tool) => {
    const family = toolFamily(tool?.name || "");
    return family !== "file_read" && family !== "search" && family !== "list_directory";
  });
  $: liveThinking = live?.thinkingByAgent?.[focusedAgentId] || "";
  $: liveText = live?.textByAgent?.[focusedAgentId] || "";
  $: queued = (live?.queued || []).filter((item) => !item.agentId || item.agentId === focusedAgentId);
  $: pendingUserMessages = live?.pendingUserMessages || [];
  $: agentLookup = Object.fromEntries((agents || []).map((agent) => [agent.agentId, agent]));
  $: workRows = liveWorkRows(live?.tools || {});

  function handleScroll() {
    if (!scroller) return;
    const threshold = 64;
    stickToBottom =
      scroller.scrollTop + scroller.clientHeight >= scroller.scrollHeight - threshold;
  }

  async function scrollToBottom(force = false) {
    if (!scroller) return;
    if (!force && !stickToBottom) return;
    await tick();
    const nextTop = scroller.scrollHeight - scroller.clientHeight;
    if (Math.abs(scroller.scrollTop - nextTop) > 2) {
      scroller.scrollTop = nextTop;
    }
  }

  $: transcriptSignature = [
    entries.length,
    liveTools.length,
    queued.length,
    liveThinking.length,
    liveText.length,
  ].join(":");

  $: if (transcriptSignature !== previousSignature) {
    previousSignature = transcriptSignature;
    scrollToBottom();
  }
</script>

<section bind:this={scroller} class="transcript" on:scroll={handleScroll}>
  {#if !entries.length && !liveThinking && !liveText && !liveTools.length && !queued.length && !pendingUserMessages.length}
    <div class="empty-state">
      <div class="empty-mark">Firmius</div>
      <h2>Ready for the next run.</h2>
      <p>
        The focused agent’s full history, live tool lifecycle, queued messages, and
        process output will appear here.
      </p>
    </div>
  {/if}

  <WorkLane title="Live Work" rows={workRows} />

  {#each pendingUserMessages as pending}
    <MessageCard
      message={{
        role: 1,
        timestamp: Date.now(),
        parts: [{ type: "text", text: pending.text }],
        pending: true,
      }}
      {live}
      {agentLookup}
      onFocusAgent={onFocusAgent}
    />
  {/each}

  {#each entries as entry}
    {#if entry.type === "message"}
      <MessageCard
        message={entry.message}
        {live}
        {agentLookup}
        onFocusAgent={onFocusAgent}
      />
    {:else if entry.type === "quickTools"}
      <article class="quick-group">
        <div class="quick-title">{entry.label}</div>
        <div class="quick-items">
          {#each entry.tools as tool}
            <div class="quick-item">
              <div class="quick-item-name">{tool.name}</div>
              {#each quickToolDetails(tool) as line}
                <div class="quick-item-detail">{line}</div>
              {/each}
            </div>
          {/each}
        </div>
      </article>
    {:else if entry.type === "tool"}
      <ToolRenderer
        tool={entry.tool}
        process={entry.tool.processId ? live?.processById?.[entry.tool.processId] : null}
        live={live}
        relatedAgent={entry.tool.subagentId ? agentLookup[entry.tool.subagentId] : null}
        onFocusAgent={onFocusAgent}
      />
    {/if}
  {/each}

  {#if liveThinking}
    <article class="live-block thinking">
      <div class="live-head">
        <span>Live Reasoning</span>
        <span>{displayAgentName(agentLookup[focusedAgentId]) || "focused agent"}</span>
      </div>
      <div class="live-body">{liveThinking}</div>
    </article>
  {/if}

  {#if liveText}
    <article class="live-block response">
      <div class="live-head">
        <span>Live Response</span>
        <span>{displayAgentName(agentLookup[focusedAgentId]) || "focused agent"}</span>
      </div>
      <div class="live-body">{liveText}</div>
    </article>
  {/if}

  {#if queued.length}
    <article class="live-block queue">
      <div class="live-head">
        <span>Queued Messages</span>
        <span>{queued.length}</span>
      </div>
      <div class="queue-list">
        {#each queued as item}
          <div class="queue-item">{item.text}</div>
        {/each}
      </div>
    </article>
  {/if}

  {#if liveSearchTools.length}
    <article class="quick-group live">
      <div class="quick-title">
        {liveSearchTools.length === 1 ? "Live quick tool" : "Live quick tools"}
      </div>
      <div class="quick-items">
        {#each liveSearchTools as tool}
          <div class="quick-item">
            <div class="quick-item-name">{tool.name}</div>
            {#each quickToolDetails(tool) as line}
              <div class="quick-item-detail">{line}</div>
            {/each}
          </div>
        {/each}
      </div>
    </article>
  {/if}

  {#each liveDetailedTools as tool}
    <ToolRenderer
      {tool}
      process={tool.processId ? live?.processById?.[tool.processId] : null}
      live={live}
      relatedAgent={tool.subagentId ? agentLookup[tool.subagentId] : null}
      onFocusAgent={onFocusAgent}
    />
  {/each}
</section>

<style>
  .transcript {
    min-height: 0;
    overflow: auto;
    display: grid;
    gap: clamp(0.65rem, 0.5rem + 0.8vw, 1rem);
    padding: clamp(0.8rem, 0.58rem + 1vw, 1.1rem) clamp(0.75rem, 0.4rem + 1.4vw, 1.2rem) clamp(0.84rem, 0.62rem + 0.9vw, 1.08rem);
    align-content: start;
    background: linear-gradient(180deg, color-mix(in srgb, var(--bg) 98%, white), color-mix(in srgb, var(--bg) 100%, black));
    overscroll-behavior: contain;
  }

  .empty-state {
    min-height: min(60vh, 34rem);
    display: grid;
    place-content: center;
    gap: 0.8rem;
    padding: 1.4rem;
    border: 1px dashed color-mix(in srgb, var(--line) 50%, transparent);
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--panel) 52%, transparent), transparent),
      radial-gradient(circle at center, color-mix(in srgb, var(--accent) 6%, transparent), transparent 55%);
    text-align: center;
  }

  .empty-mark {
    color: var(--muted);
    font: 700 0.68rem/1 var(--font-display);
    letter-spacing: 0.22em;
    text-transform: uppercase;
  }

  .empty-state h2 {
    margin: 0;
    font: 800 clamp(1.1rem, 2vw, 1.65rem)/1 var(--font-display);
  }

  .empty-state p {
    margin: 0;
    max-width: 54ch;
    color: var(--muted);
    line-height: 1.6;
  }

  .live-block {
    display: grid;
    gap: clamp(0.46rem, 0.38rem + 0.4vw, 0.6rem);
    padding: clamp(0.62rem, 0.52rem + 0.55vw, 0.78rem) clamp(0.66rem, 0.54rem + 0.65vw, 0.86rem);
    border: 1px solid var(--line-soft);
    background: linear-gradient(180deg, color-mix(in srgb, var(--panel-strong) 90%, white), color-mix(in srgb, var(--panel) 88%, transparent));
  }

  .quick-group {
    display: grid;
    gap: clamp(0.46rem, 0.38rem + 0.4vw, 0.6rem);
    padding: clamp(0.62rem, 0.52rem + 0.55vw, 0.8rem) clamp(0.66rem, 0.54rem + 0.65vw, 0.86rem);
    border: 1px solid var(--line-soft);
    background: linear-gradient(180deg, color-mix(in srgb, var(--panel-strong) 90%, white), color-mix(in srgb, var(--panel) 88%, transparent));
  }

  .quick-title {
    color: var(--muted);
    font-size: 0.66rem;
    font-weight: 700;
    letter-spacing: 0.12em;
    text-transform: uppercase;
  }

  .quick-items {
    display: grid;
    gap: 0.45rem;
  }

  .quick-item {
    display: grid;
    gap: 0.25rem;
    padding: 0.58rem 0.68rem;
    border-left: 2px solid color-mix(in srgb, var(--tool-read) 28%, var(--line-soft));
    background: color-mix(in srgb, var(--panel) 82%, white);
    min-width: 0;
  }

  .quick-item-name {
    color: var(--muted);
    font-size: 0.63rem;
    font-weight: 700;
    letter-spacing: 0.1em;
    text-transform: uppercase;
  }

  .quick-item-detail {
    font: 0.8rem/1.45 var(--font-mono);
    white-space: pre-wrap;
    overflow-wrap: anywhere;
  }

  .live-head {
    display: flex;
    justify-content: space-between;
    gap: 1rem;
    flex-wrap: wrap;
    font-size: 0.6rem;
    font-weight: 700;
    letter-spacing: 0.16em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .live-body,
  .queue-item {
    white-space: pre-wrap;
    line-height: 1.58;
    font-size: 0.82rem;
  }

  .queue-list {
    display: grid;
    gap: 0.45rem;
  }

  .queue-item {
    padding-left: 0.66rem;
    border-left: 2px solid color-mix(in srgb, var(--warn) 34%, var(--line-soft));
  }

  @media (max-width: 820px) {
    .transcript {
      padding: 0.75rem 0.7rem 0.92rem;
      gap: 0.45rem;
    }

    .quick-group,
    .live-block {
      padding: 0.66rem 0.7rem;
    }
  }
</style>
