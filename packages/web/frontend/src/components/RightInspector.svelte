<script>
  import StatusPill from "./StatusPill.svelte";
  import { agentStatus, displayAgentName } from "../lib/transcript.js";

  export let thread = null;
  export let agents = [];
  export let focusedAgentId = "";
  export let live = null;
  export let themeName = "";
  export let themes = [];
  export let hasMultipleAgents = false;
  export let canFocusParent = false;
  export let onSelectTheme = null;
  export let onFocusAgent = null;
  export let onFocusParent = null;
  export let onCyclePrevious = null;
  export let onCycleNext = null;
  export let visible = true;
  export let mobile = false;
  export let onClose = null;

  $: focusedAgent = agents.find((agent) => agent.agentId === focusedAgentId) || null;
  $: listedAgents = focusedAgent ? agents.filter((agent) => agent.agentId !== focusedAgentId) : agents;
</script>

<aside class:mobile class:visible class="inspector">
  <header class="inspector-head">
    <div>
      <div class="eyebrow">Operator</div>
      <h3>{thread?.title || "No thread"}</h3>
    </div>
    {#if mobile}
      <button class="close-button" on:click={onClose}>Close</button>
    {/if}
  </header>

  <section class="panel">
    <div class="panel-title">Theme</div>
    <select value={themeName} on:change={(event) => onSelectTheme?.(event.currentTarget.value)}>
      {#each themes as theme}
        <option value={theme.name}>{theme.name}</option>
      {/each}
    </select>
  </section>

  {#if focusedAgent}
    <section class="panel">
      <div class="panel-title">Focused Agent</div>
      <div class="agent-card active pinned">
        <div class="agent-header">
          <div>
            <div class="agent-name">{displayAgentName(focusedAgent)}</div>
            <div class="agent-meta">{[focusedAgent.persona, focusedAgent.modelId].filter(Boolean).join("  •  ")}</div>
          </div>
          <StatusPill
            tone={agentStatus(focusedAgent, live) === "idle" ? "muted" : "active"}
            label={agentStatus(focusedAgent, live)}
          />
        </div>
        <div class="agent-submeta">
          {[thread?.leadPersona || "", thread?.permissionMode || ""].filter(Boolean).join("  •  ")}
        </div>
        {#if hasMultipleAgents}
          <div class="action-grid">
            <button on:click={onFocusParent} disabled={!canFocusParent}>Parent</button>
            <button on:click={onCyclePrevious}>Prev</button>
            <button on:click={onCycleNext}>Next</button>
          </div>
        {/if}
      </div>
    </section>
  {/if}

  {#if listedAgents.length}
    <section class="panel grow">
      <div class="panel-title">Agents</div>
      <div class="agent-list">
        {#each listedAgents as agent}
          <button class="agent-card" on:click={() => onFocusAgent?.(agent.agentId)}>
            <div class="agent-header">
              <div>
                <div class="agent-name">{displayAgentName(agent)}</div>
                <div class="agent-meta">{[agent.persona, agent.modelId].filter(Boolean).join("  •  ")}</div>
              </div>
              <StatusPill
                tone={agentStatus(agent, live) === "idle" ? "muted" : "active"}
                label={agentStatus(agent, live)}
              />
            </div>
            {#if agent.parentId}
              <div class="agent-submeta">child of {agent.parentId.slice(0, 8)}</div>
            {/if}
          </button>
        {/each}
      </div>
    </section>
  {/if}
</aside>

<style>
  .inspector {
    display: flex;
    flex-direction: column;
    gap: 0.78rem;
    min-width: 0;
    height: 100%;
    padding: 0.95rem 0.9rem 0.82rem;
    border-left: 1px solid var(--line-soft);
    background: linear-gradient(180deg, color-mix(in srgb, var(--panel) 94%, white), color-mix(in srgb, var(--bg) 98%, black));
    overflow: hidden;
  }

  .inspector.mobile {
    position: fixed;
    inset: 0 0 0 auto;
    width: min(90vw, 360px);
    z-index: 40;
    transform: translateX(100%);
    transition: transform 180ms ease;
    box-shadow: -24px 0 60px rgba(0, 0, 0, 0.28);
  }

  .inspector.mobile.visible {
    transform: translateX(0);
  }

  .inspector-head,
  .agent-header,
  .action-grid {
    display: flex;
    align-items: start;
    justify-content: space-between;
    gap: 0.5rem;
  }

  h3,
  .panel-title,
  .agent-name {
    margin: 0;
  }

  .eyebrow,
  .agent-meta,
  .agent-submeta {
    color: var(--muted);
    font-size: 0.64rem;
    line-height: 1.35;
  }

  .eyebrow,
  .panel-title {
    font-weight: 700;
    letter-spacing: 0.17em;
    text-transform: uppercase;
  }

  .panel {
    display: grid;
    gap: 0.52rem;
    padding: 0.68rem 0.72rem;
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel-strong) 84%, transparent);
  }

  .grow {
    min-height: 0;
  }

  .action-grid {
    display: grid;
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }

  .action-grid button,
  .close-button,
  select,
  .agent-card {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 78%, white);
    color: var(--fg);
    font: inherit;
  }

  .action-grid button,
  .close-button,
  select {
    min-height: 2rem;
    padding: 0.46rem 0.56rem;
    font-size: 0.74rem;
  }

  .agent-list {
    display: grid;
    gap: 0.46rem;
    min-height: 0;
    overflow: auto;
  }

  .agent-card {
    display: grid;
    gap: 0.4rem;
    text-align: left;
    padding: 0.62rem;
    cursor: pointer;
    min-width: 0;
    overflow: hidden;
  }

  .agent-card.active,
  .agent-card.pinned {
    border-color: color-mix(in srgb, var(--accent) 24%, var(--line-soft));
    background: color-mix(in srgb, var(--accent) 6%, var(--panel));
  }



  .agent-meta,
  .agent-submeta,
  .agent-name {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  @media (max-width: 1100px) {
    .inspector:not(.mobile) {
      display: none;
    }
  }
</style>
