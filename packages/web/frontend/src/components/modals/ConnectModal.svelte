<script>
  import { onDestroy } from "svelte";
  import { api } from "../../lib/api.js";
  import ModalFrame from "./ModalFrame.svelte";

  export let providers = [];
  export let onClose = null;

  let selectedProviderId = "";
  let loading = false;
  let answer = "";
  let wizard = null;
  let pollTimer = null;
  let error = "";

  $: connectableProviders = (providers || []).filter(
    (provider) => provider?.supportsConnectWizard,
  );
  $: if (
    (!selectedProviderId ||
      !connectableProviders.some((provider) => provider.id === selectedProviderId)) &&
    connectableProviders[0]?.id
  ) {
    selectedProviderId = connectableProviders[0].id;
  }
  $: selectedProvider =
    connectableProviders.find((provider) => provider.id === selectedProviderId) ||
    null;
  $: wizardUrl = extractUrl(wizard?.prompt || "");
  $: wizardPending = Boolean(wizard && !wizard.complete);
  $: wizardDone = Boolean(wizard?.complete);
  $: wizardBusy = loading || (wizardPending && wizard?.wizardType === "OAuth");

  onDestroy(() => {
    stopPolling();
  });

  function extractUrl(text) {
    const match = String(text || "").match(/https?:\/\/[^\s]+/);
    return match ? match[0] : "";
  }

  function stopPolling() {
    if (pollTimer) {
      clearTimeout(pollTimer);
      pollTimer = null;
    }
  }

  function schedulePoll() {
    stopPolling();
    pollTimer = setTimeout(checkWizardStatus, 700);
  }

  async function startWizard(providerId) {
    if (!providerId) return;
    loading = true;
    error = "";
    answer = "";
    stopPolling();
    try {
      const response = await api.startConnectWizard(providerId);
      wizard = response;
      if (!response.complete && response.wizardType === "OAuth") {
        schedulePoll();
      }
    } catch (err) {
      wizard = null;
      error = err.message;
    } finally {
      loading = false;
    }
  }

  async function checkWizardStatus() {
    if (!wizard?.sessionId || wizard.complete) return;
    try {
      const response = await api.getConnectWizardStatus(wizard.sessionId);
      wizard = response;
      if (!response.complete && response.wizardType === "OAuth") {
        schedulePoll();
      }
    } catch (err) {
      error = err.message;
      stopPolling();
    }
  }

  async function submitWizard() {
    if (!wizard?.sessionId || loading) return;
    loading = true;
    error = "";
    stopPolling();
    try {
      const response = await api.submitConnectWizard(wizard.sessionId, answer);
      wizard = response;
      answer = "";
      if (!response.complete && response.wizardType === "OAuth") {
        schedulePoll();
      }
    } catch (err) {
      error = err.message;
    } finally {
      loading = false;
    }
  }

  async function cancelWizard() {
    stopPolling();
    if (wizard?.sessionId) {
      try {
        await api.cancelConnectWizard(wizard.sessionId);
      } catch {
        // Best-effort cleanup only.
      }
    }
    wizard = null;
    answer = "";
    error = "";
  }

  function handleProviderChange(event) {
    selectedProviderId = event.currentTarget.value;
    wizard = null;
    answer = "";
    error = "";
    stopPolling();
  }
</script>

<ModalFrame title="Connect" {onClose}>
  <div class="layout">
    <section class="providers">
      <div class="section-title">Providers</div>
      {#if connectableProviders.length}
        {#each connectableProviders as provider}
          <button
            class:selected={provider.id === selectedProviderId}
            class="provider-row"
            type="button"
            on:click={() => {
              selectedProviderId = provider.id;
              wizard = null;
              answer = "";
              error = "";
              stopPolling();
            }}
          >
            <span class="provider-name">{provider.id}</span>
            <span class={`provider-kind ${provider.connectWizardType?.toLowerCase() || ""}`}>
              {provider.connectWizardType || provider.type}
            </span>
          </button>
        {/each}
      {:else}
        <div class="empty">No interactive provider connection flows are available.</div>
      {/if}
    </section>

    <section class="wizard">
      <div class="section-title">Connection Flow</div>
      {#if selectedProvider}
        <div class="wizard-header">
          <div>
            <div class="provider-label">{selectedProvider.id}</div>
            <div class="provider-meta">
              {selectedProvider.connectWizardType || selectedProvider.type}
              {#if selectedProvider.configured}
                · configured
              {:else}
                · not configured
              {/if}
            </div>
          </div>
          <div class="header-actions">
            <select bind:value={selectedProviderId} on:change={handleProviderChange}>
              {#each connectableProviders as provider}
                <option value={provider.id}>{provider.id}</option>
              {/each}
            </select>
            <button type="button" on:click={() => startWizard(selectedProviderId)} disabled={loading}>
              {wizard ? "Restart" : "Start"}
            </button>
          </div>
        </div>

        {#if error}
          <div class="status error">{error}</div>
        {/if}

        {#if !wizard}
          <div class="panel muted">
            Start the provider wizard to use the same base OAuth or API-key setup flow the TUI uses.
          </div>
        {:else if wizardDone}
          <div class={`panel ${wizard.success ? "success" : "error"}`}>
            <div class="result-title">{wizard.success ? "Connection complete" : "Connection failed"}</div>
            <div>{wizard.success ? wizard.message : wizard.error}</div>
          </div>
        {:else}
          <div class="panel">
            <div class="prompt">{wizard.prompt || "Waiting for provider prompt..."}</div>

            {#if wizardUrl}
              <div class="oauth-actions">
                <a href={wizardUrl} target="_blank" rel="noreferrer">Open Browser</a>
                <button type="button" on:click={checkWizardStatus} disabled={loading}>Refresh Status</button>
              </div>
            {/if}

            {#if wizard.wizardType === "APIKey" || wizard.isSecret}
              <label class="input-block">
                <span>Answer</span>
                <input
                  type={wizard.isSecret ? "password" : "text"}
                  bind:value={answer}
                  placeholder={wizard.isSecret ? "Enter secret…" : "Enter response…"}
                  on:keydown={(event) => {
                    if (event.key === "Enter") {
                      event.preventDefault();
                      submitWizard();
                    }
                  }}
                />
              </label>
              <div class="inline-actions">
                <button type="button" on:click={submitWizard} disabled={loading || !answer.trim()}>
                  Submit
                </button>
                <button type="button" class="ghost" on:click={cancelWizard}>Cancel</button>
              </div>
            {:else}
              <div class="panel muted compact">
                Waiting for the provider to finish authentication. Keep the external auth page open, then refresh status if needed.
              </div>
              <div class="inline-actions">
                <button type="button" class="ghost" on:click={cancelWizard}>Cancel</button>
              </div>
            {/if}
          </div>
        {/if}
      {:else}
        <div class="empty">No provider selected.</div>
      {/if}
    </section>
  </div>
</ModalFrame>

<style>
  .layout {
    display: grid;
    grid-template-columns: minmax(12rem, 15rem) minmax(0, 1fr);
    gap: 1rem;
    min-height: min(34rem, 70vh);
  }
  .providers,
  .wizard {
    display: grid;
    align-content: start;
    gap: 0.75rem;
  }
  .section-title,
  .provider-meta,
  .provider-kind,
  .provider-label,
  .result-title,
  .input-block span {
    text-transform: uppercase;
    letter-spacing: 0.14em;
  }
  .section-title,
  .provider-label,
  .result-title,
  .input-block span {
    font-size: 0.68rem;
    font-weight: 700;
    color: var(--muted);
  }
  .provider-row,
  .panel,
  select,
  input,
  button,
  .status,
  .empty {
    border: 1px solid var(--line-soft);
    background: color-mix(in srgb, var(--panel) 88%, white);
    color: var(--fg);
  }
  .provider-row {
    display: flex;
    justify-content: space-between;
    gap: 0.75rem;
    width: 100%;
    padding: 0.8rem;
    text-align: left;
  }
  .provider-row.selected {
    border-color: color-mix(in srgb, var(--accent) 55%, var(--line-soft));
    background: color-mix(in srgb, var(--panel-strong) 90%, var(--accent) 10%);
  }
  .provider-name {
    font-weight: 600;
  }
  .provider-kind,
  .provider-meta {
    font-size: 0.62rem;
    color: var(--muted);
  }
  .wizard-header {
    display: flex;
    justify-content: space-between;
    gap: 0.75rem;
    align-items: start;
  }
  .header-actions,
  .oauth-actions,
  .inline-actions {
    display: flex;
    gap: 0.5rem;
    flex-wrap: wrap;
  }
  .panel,
  .status,
  .empty {
    padding: 0.9rem;
  }
  .panel {
    display: grid;
    gap: 0.75rem;
  }
  .panel.compact {
    padding-top: 0.7rem;
    padding-bottom: 0.7rem;
  }
  .panel.muted,
  .empty {
    color: var(--muted);
  }
  .panel.success {
    border-color: color-mix(in srgb, var(--ok) 48%, var(--line-soft));
  }
  .panel.error,
  .status.error {
    border-color: color-mix(in srgb, var(--error) 48%, var(--line-soft));
  }
  .prompt {
    white-space: pre-wrap;
    line-height: 1.5;
  }
  .input-block {
    display: grid;
    gap: 0.45rem;
  }
  select,
  input,
  button,
  a {
    min-height: 2.2rem;
    padding: 0.5rem 0.65rem;
    font: inherit;
  }
  a {
    display: inline-flex;
    align-items: center;
    text-decoration: none;
  }
  button.ghost {
    background: color-mix(in srgb, var(--panel) 75%, transparent);
  }
  @media (max-width: 820px) {
    .layout {
      grid-template-columns: 1fr;
      min-height: auto;
    }
    .wizard-header {
      flex-direction: column;
    }
    .header-actions {
      width: 100%;
    }
    .header-actions select,
    .header-actions button {
      flex: 1 1 10rem;
    }
  }
</style>
