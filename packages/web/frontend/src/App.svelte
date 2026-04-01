<script>
  import { onDestroy, onMount } from "svelte";
  import Composer from "./components/Composer.svelte";
  import Header from "./components/Header.svelte";
  import AccountsModal from "./components/modals/AccountsModal.svelte";
  import ConnectModal from "./components/modals/ConnectModal.svelte";
  import PurposesModal from "./components/modals/PurposesModal.svelte";
  import QuotasModal from "./components/modals/QuotasModal.svelte";
  import RightInspector from "./components/RightInspector.svelte";
  import RouterModal from "./components/modals/RouterModal.svelte";
  import Sidebar from "./components/Sidebar.svelte";
  import Transcript from "./components/Transcript.svelte";
  import { api } from "./lib/api.js";
  import { appStore, viewState } from "./stores/app.js";

  let state = { snapshot: null, live: null, themes: [], themeName: "" };
  let leftOpen = true;
  let rightOpen = true;
  let mobileSidebarOpen = false;
  let mobileInspectorOpen = false;
  let activeModal = "";
  let modalConfig = null;
  let modalPurposes = [];
  let modalProviders = [];

  const unsubscribe = viewState.subscribe((value) => {
    state = value;
  });
  const unsubscribeError = appStore.error.subscribe((message) => {
    if (message) console.error(message);
  });

  onMount(() => {
    (async () => {
      await appStore.loadThemes();
      await appStore.refreshState();
    })().catch((error) => console.error(error));

    const handleKeydown = (event) => {
      const target = event.target;
      const editing =
        target instanceof HTMLInputElement ||
        target instanceof HTMLTextAreaElement ||
        target instanceof HTMLSelectElement ||
        target?.isContentEditable;
      if (editing) return;

      if (event.altKey && event.key === "[") {
        event.preventDefault();
        focusPreviousAgent();
      } else if (event.altKey && event.key === "]") {
        event.preventDefault();
        focusNextAgent();
      } else if (event.altKey && event.key === "ArrowLeft") {
        event.preventDefault();
        focusParentAgent();
      } else if (event.altKey && event.key.toLowerCase() === "b") {
        event.preventDefault();
        leftOpen = !leftOpen;
      } else if (event.altKey && event.key.toLowerCase() === "i") {
        event.preventDefault();
        rightOpen = !rightOpen;
      }
    };

    window.addEventListener("keydown", handleKeydown);
    return () => window.removeEventListener("keydown", handleKeydown);
  });

  onDestroy(() => {
    unsubscribe();
    unsubscribeError();
    appStore.disconnectStream();
  });

  $: snapshot = state.snapshot || {};
  $: live = state.live || {};
  $: thread = snapshot.thread || null;
  $: agents = snapshot.agents || [];
  $: focusedAgent =
    agents.find((agent) => agent.agentId === snapshot.focusedAgentId) || null;
  $: modelOptions = (snapshot.models || []).map((model) => ({
    value: `${model.providerId}::${model.id}`,
    label: `${model.providerId} / ${model.id}`,
  }));
  $: currentModel = focusedAgent
    ? `${focusedAgent.providerId}::${focusedAgent.modelId}`
    : modelOptions[0]?.value || "";
  $: currentModelInfo =
    (snapshot.models || []).find(
      (model) => `${model.providerId}::${model.id}` === currentModel,
    ) || null;
  $: note = [
    live.providerWaiting?.[snapshot.focusedAgentId] ? "provider waiting" : "",
    live.retryByAgent?.[snapshot.focusedAgentId] || "",
    live.streamMessage || "",
  ]
    .filter(Boolean)
    .join("  •  ");
  $: queuedCount = (live.queued || []).filter(
    (entry) => !entry.agentId || entry.agentId === snapshot.focusedAgentId,
  ).length;
  $: hasMultipleAgents = agents.length > 1;
  $: canFocusParent = Boolean(focusedAgent?.parentId);
  $: isFocusedAgentBusy =
    Boolean(live?.providerWaiting?.[snapshot.focusedAgentId]) ||
    Boolean(live?.retryByAgent?.[snapshot.focusedAgentId]) ||
    Boolean(live?.thinkingByAgent?.[snapshot.focusedAgentId]) ||
    Boolean(live?.textByAgent?.[snapshot.focusedAgentId]) ||
    Boolean(focusedAgent?.isRunning) ||
    Boolean(focusedAgent?.isBooting);

  function closeMobilePanels() {
    mobileSidebarOpen = false;
    mobileInspectorOpen = false;
  }

  function desktopMode() {
    return typeof window !== "undefined" && window.innerWidth > 1100;
  }

  function toggleSidebar() {
    if (desktopMode()) {
      leftOpen = !leftOpen;
      return;
    }
    mobileSidebarOpen = true;
  }

  function toggleInspector() {
    if (desktopMode()) {
      rightOpen = !rightOpen;
      return;
    }
    mobileInspectorOpen = true;
  }

  function switchThread(threadId) {
    closeMobilePanels();
    return appStore.switchThread(threadId);
  }

  function focusAgent(agentId) {
    closeMobilePanels();
    return appStore.focusAgent(agentId);
  }

  function focusParentAgent() {
    if (focusedAgent?.parentId) {
      focusAgent(focusedAgent.parentId);
    }
  }

  function cycleAgent(delta) {
    if (!agents.length) return;
    const currentIndex = Math.max(
      0,
      agents.findIndex((agent) => agent.agentId === snapshot.focusedAgentId),
    );
    const nextIndex = (currentIndex + delta + agents.length) % agents.length;
    focusAgent(agents[nextIndex].agentId);
  }

  function focusPreviousAgent() {
    cycleAgent(-1);
  }

  function focusNextAgent() {
    cycleAgent(1);
  }

  function handleSend(text) {
    const trimmed = String(text || "").trim();
    if (!trimmed) return;
    if (trimmed.startsWith("/")) {
      const [workflowToken, ...args] = trimmed.slice(1).split(/\s+/);
      if (workflowToken) {
        return appStore.executeWorkflow(workflowToken, args);
      }
    }
    return appStore.sendMessage(trimmed);
  }

  async function openModal(kind) {
    activeModal = kind;
    if (kind === "router" || kind === "purposes" || kind === "connect") {
      const response = await api.config();
      modalConfig = response.config || {};
      modalPurposes = response.purposes || [];
      if (kind === "router" || kind === "connect") {
        const providers = await api.providers();
        modalProviders = providers.providers || [];
      }
      return;
    }
    if (kind === "accounts" || kind === "quotas") {
      const providers = await api.providers();
      modalProviders = providers.providers || [];
    }
  }

  function closeModal() {
    activeModal = "";
  }
</script>

<div class={`app-shell ${leftOpen ? "left-open" : "left-closed"} ${rightOpen ? "right-open" : "right-closed"}`}>
  <Sidebar
    threads={snapshot.threads || []}
    currentThreadId={snapshot.currentThreadId || ""}
    onNewThread={() => appStore.createThread()}
    onSwitchThread={switchThread}
    onDeleteThread={(threadId) => appStore.deleteThread(threadId)}
  />

  <main class="chat-frame">
    <Header
      {thread}
      {focusedAgent}
      {live}
      {queuedCount}
      {hasMultipleAgents}
      onToggleSidebar={toggleSidebar}
      onToggleInspector={toggleInspector}
      onOpenRouter={() => openModal("router")}
      onOpenPurposes={() => openModal("purposes")}
      onOpenAccounts={() => openModal("accounts")}
      onOpenQuotas={() => openModal("quotas")}
      onOpenConnect={() => openModal("connect")}
    />

    <Transcript
      history={snapshot.focusedHistory}
      {live}
      {agents}
      focusedAgentId={snapshot.focusedAgentId || ""}
      onFocusAgent={focusAgent}
    />

    <Composer
      {modelOptions}
      {currentModel}
      currentVariant={focusedAgent?.modelVariant || ""}
      variants={currentModelInfo?.variants || []}
      {note}
      running={isFocusedAgentBusy}
      onSend={handleSend}
      onInterrupt={() => appStore.interrupt()}
      onRetry={() => appStore.retryLast()}
      onUndo={() => appStore.undo({ count: 1 })}
      onCompact={() => appStore.compact()}
      onRefresh={() => appStore.refreshState({ keepLive: true })}
      onSwitchModel={(value, variant) => appStore.switchModel(value, variant)}
    />
  </main>

  <RightInspector
    {thread}
    {agents}
    focusedAgentId={snapshot.focusedAgentId || ""}
    {live}
    themeName={state.themeName}
    themes={state.themes}
    {hasMultipleAgents}
    {canFocusParent}
    onSelectTheme={(name) => appStore.selectTheme(name)}
    onFocusAgent={focusAgent}
    onFocusParent={focusParentAgent}
    onCyclePrevious={focusPreviousAgent}
    onCycleNext={focusNextAgent}
  />

  <Sidebar
    mobile={true}
    visible={mobileSidebarOpen}
    threads={snapshot.threads || []}
    currentThreadId={snapshot.currentThreadId || ""}
    onNewThread={() => appStore.createThread()}
    onSwitchThread={switchThread}
    onDeleteThread={(threadId) => appStore.deleteThread(threadId)}
    onClose={closeMobilePanels}
  />

  <RightInspector
    mobile={true}
    visible={mobileInspectorOpen}
    {thread}
    {agents}
    focusedAgentId={snapshot.focusedAgentId || ""}
    {live}
    themeName={state.themeName}
    themes={state.themes}
    {hasMultipleAgents}
    {canFocusParent}
    onSelectTheme={(name) => appStore.selectTheme(name)}
    onFocusAgent={focusAgent}
    onFocusParent={focusParentAgent}
    onCyclePrevious={focusPreviousAgent}
    onCycleNext={focusNextAgent}
    onClose={closeMobilePanels}
  />

  {#if mobileSidebarOpen || mobileInspectorOpen}
    <button class="overlay" on:click={closeMobilePanels} aria-label="Close panels"></button>
  {/if}

  {#if activeModal === "router"}
    <RouterModal
      config={modalConfig}
      providers={modalProviders}
      onSave={(config) => appStore.updateConfig(config).then(closeModal)}
      onClose={closeModal}
    />
  {:else if activeModal === "purposes"}
    <PurposesModal
      config={modalConfig}
      purposes={modalPurposes}
      onSave={(config) => appStore.updateConfig(config).then(closeModal)}
      onClose={closeModal}
    />
  {:else if activeModal === "accounts"}
    <AccountsModal
      providers={modalProviders}
      onDeleteAccount={(providerId, identifier) => appStore.deleteAccount(providerId, identifier).then(() => openModal("accounts"))}
      onClose={closeModal}
    />
  {:else if activeModal === "quotas"}
    <QuotasModal providers={modalProviders} onClose={closeModal} />
  {:else if activeModal === "connect"}
    <ConnectModal
      providers={modalProviders}
      onClose={closeModal}
    />
  {/if}
</div>

<style>
  :global(:root) {
    --bg: #16181c;
    --fg: #ebe4d7;
    --line: rgba(126, 118, 105, 0.28);
    --line-soft: rgba(126, 118, 105, 0.16);
    --accent: #d5ad68;
    --muted: #968b7d;
    --panel: rgba(26, 29, 34, 0.9);
    --panel-strong: rgba(21, 24, 29, 0.98);
    --user: #7bc4c4;
    --assistant: #d7a4bc;
    --timestamp: #857b6e;
    --code-bg: #101318;
    --code-fg: #efe6d8;
    --tool-read: #67a7d6;
    --tool-edit: #8fbf7b;
    --tool-process: #db9c57;
    --tool-subagent: #b79fff;
    --tool-work: #f0cf77;
    --ok: #5bd18b;
    --warn: #f0bc5e;
    --error: #ef6e64;
    --font-display: "Space Grotesk", "Avenir Next Condensed", "Segoe UI", sans-serif;
    --font-sans: "IBM Plex Sans", "Segoe UI", sans-serif;
    --font-mono: "IBM Plex Mono", "JetBrains Mono", monospace;
    --panel-shadow: 0 8px 18px rgba(0, 0, 0, 0.18);
  }

  :global(html, body, #app) {
    margin: 0;
    width: 100%;
    height: 100%;
    overflow: hidden;
  }

  :global(html) {
    color-scheme: dark;
  }

  :global(body) {
    background:
      radial-gradient(circle at top left, color-mix(in srgb, var(--accent) 8%, transparent), transparent 24%),
      linear-gradient(180deg, #0f1114, color-mix(in srgb, var(--bg) 96%, black));
    color: var(--fg);
    font: 400 14px/1.45 var(--font-sans);
  }

  :global(button),
  :global(select),
  :global(textarea),
  :global(input) {
    appearance: none;
    border-radius: 0;
    outline: none;
  }

  :global(*),
  :global(*::before),
  :global(*::after) {
    box-sizing: border-box;
    min-width: 0;
  }

  :global(button:focus-visible),
  :global(select:focus-visible),
  :global(textarea:focus-visible),
  :global(input:focus-visible) {
    box-shadow: 0 0 0 1px color-mix(in srgb, var(--accent) 56%, white),
      0 0 0 4px color-mix(in srgb, var(--accent) 18%, transparent);
  }

  :global(button),
  :global(a),
  :global(select) {
    touch-action: manipulation;
    -webkit-tap-highlight-color: color-mix(in srgb, var(--accent) 22%, transparent);
  }

  :global(textarea),
  :global(input),
  :global(select) {
    font-size: max(16px, 1rem);
  }

  :global(*::-webkit-scrollbar) {
    width: 10px;
    height: 10px;
  }

  :global(*::-webkit-scrollbar-track) {
    background: transparent;
  }

  :global(*::-webkit-scrollbar-thumb) {
    background: linear-gradient(180deg, color-mix(in srgb, var(--muted) 70%, transparent), color-mix(in srgb, var(--line) 78%, transparent));
    border: 2px solid transparent;
    background-clip: padding-box;
  }

  :global(*) {
    scrollbar-width: thin;
    scrollbar-color: color-mix(in srgb, var(--muted) 70%, transparent) transparent;
  }

  .app-shell {
    display: grid;
    grid-template-columns: clamp(12rem, 18vw, 15.5rem) minmax(0, 1fr) clamp(13rem, 19vw, 17rem);
    width: 100%;
    height: 100%;
  }

  .app-shell.left-closed {
    grid-template-columns: 0 minmax(0, 1fr) clamp(13rem, 19vw, 17rem);
  }

  .app-shell.right-closed {
    grid-template-columns: clamp(12rem, 18vw, 15.5rem) minmax(0, 1fr) 0;
  }

  .app-shell.left-closed.right-closed {
    grid-template-columns: 0 minmax(0, 1fr) 0;
  }

  .chat-frame {
    min-width: 0;
    min-height: 0;
    display: grid;
    grid-template-rows: auto minmax(0, 1fr) auto;
    border-inline: 1px solid var(--line-soft);
    background:
      linear-gradient(180deg, color-mix(in srgb, var(--bg) 98%, white), color-mix(in srgb, var(--bg) 100%, black));
  }

  .overlay {
    position: fixed;
    inset: 0;
    border: 0;
    background: rgba(6, 8, 10, 0.5);
    z-index: 30;
  }

  @media (max-width: 1100px) {
    .app-shell,
    .app-shell.left-closed,
    .app-shell.right-closed,
    .app-shell.left-closed.right-closed {
      grid-template-columns: minmax(0, 1fr);
    }

    .chat-frame {
      border-inline: none;
    }
  }
</style>
