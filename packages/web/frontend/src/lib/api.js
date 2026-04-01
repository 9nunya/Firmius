async function request(path, options = {}) {
  const response = await fetch(path, {
    method: options.method || "GET",
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
    body: options.body ? JSON.stringify(options.body) : undefined,
  });
  const payload = await response.json();
  if (!response.ok || payload.ok === false) {
    throw new Error(payload.error || `Request failed: ${response.status}`);
  }
  return payload;
}

export const api = {
  state: () => request("/api/state"),
  themes: () => request("/api/themes"),
  config: () => request("/api/config"),
  providers: () => request("/api/providers"),
  startConnectWizard: (providerId) =>
    request("/api/connect/wizard/start", {
      method: "POST",
      body: { providerId },
    }),
  getConnectWizardStatus: (sessionId) =>
    request(`/api/connect/wizard/status?sessionId=${encodeURIComponent(sessionId)}`),
  submitConnectWizard: (sessionId, answer = "") =>
    request("/api/connect/wizard/submit", {
      method: "POST",
      body: { sessionId, answer },
    }),
  cancelConnectWizard: (sessionId) =>
    request("/api/connect/wizard/cancel", {
      method: "POST",
      body: { sessionId },
    }),
  workflows: () => request("/api/workflows"),
  work: () => request("/api/work"),
  createThread: (body = {}) => request("/api/threads/new", { method: "POST", body }),
  switchThread: (threadId) =>
    request("/api/threads/switch", { method: "POST", body: { threadId } }),
  deleteThread: (threadId) =>
    request("/api/threads/delete", { method: "POST", body: { threadId } }),
  switchLeadPersona: (leadPersona) =>
    request("/api/threads/lead", { method: "POST", body: { leadPersona } }),
  focusAgent: (agentId) =>
    request("/api/agents/focus", { method: "POST", body: { agentId } }),
  switchModel: (providerId, modelId, variant) =>
    request("/api/models/switch", {
      method: "POST",
      body: { providerId, modelId, variant },
    }),
  updateConfig: (config) =>
    request("/api/config/update", { method: "POST", body: { config } }),
  updateRouter: (body) =>
    request("/api/router/update", { method: "POST", body }),
  updatePurposes: (body) =>
    request("/api/purposes/update", { method: "POST", body }),
  sendMessage: (text) =>
    request("/api/messages/send", { method: "POST", body: { text } }),
  retryLast: () => request("/api/messages/retry", { method: "POST" }),
  undo: (body = {}) => request("/api/messages/undo", { method: "POST", body }),
  compact: () => request("/api/messages/compact", { method: "POST" }),
  interrupt: () => request("/api/messages/interrupt", { method: "POST" }),
  respondPermission: (requestId, response) =>
    request("/api/permissions/respond", {
      method: "POST",
      body: { requestId, response },
    }),
  setPermissionMode: (mode) =>
    request("/api/permissions/mode", { method: "POST", body: { mode } }),
  deleteAccount: (providerId, identifier) =>
    request("/api/accounts/delete", {
      method: "POST",
      body: { providerId, identifier },
    }),
  executeWorkflow: (workflowId, args = []) =>
    request("/api/workflows/execute", {
      method: "POST",
      body: { workflowId, args },
    }),
};
