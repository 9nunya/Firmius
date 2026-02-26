import type { Thread, ThreadResponse, Message } from "./types";

export function normalizeThread(response: ThreadResponse): Thread {
  return {
    id: response.id,
    title: response.title,
    rootCwd: response.rootCwd,
    leadAgentId: response.leadAgentId,
    checkpointedAt: new Date(response.checkpointedAt),
    agentCount: response.agentCount,
    tokensLimit: response.tokensLimit,
    tokensUsed: response.tokensUsed,
    modelId: response.modelId,
    providerId: response.providerId,
    reasoningEffort: response.reasoningEffort,
  };
}

export function normalizeMessage(raw: Record<string, unknown>): Message {
  return {
    sequence: Number(raw.sequence) || 0,
    isUser: Boolean(raw.isUser),
    content: raw.content as string | unknown[],
    timestamp: new Date(raw.timestamp as string | number | Date),
    tokens: Number(raw.tokens) || 0,
    agentId: raw.agentId as string | undefined,
    type: (raw.type as Message["type"]) || "monologue",
    isMonologue: raw.isMonologue as boolean | undefined,
    thinking: raw.thinking as string | undefined,
    isStreaming: raw.isStreaming as boolean | undefined,
    providerRequest: raw.providerRequest as Record<string, unknown> | undefined,
    providerError: raw.providerError as
      | { error: string; modelId?: string; providerId?: string }
      | undefined,
    toolCalls: (raw.toolCalls as Message["toolCalls"]) || undefined,
  };
}

export function normalizeThreads(responses: ThreadResponse[]): Thread[] {
  return responses.map(normalizeThread);
}

export function normalizeMessages(rawMessages: Record<string, unknown>[]): Message[] {
  return rawMessages.map(normalizeMessage);
}
