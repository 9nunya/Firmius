import type { ModelInfo } from "./ModelInfo";
import type { StreamChunk } from "./StreamChunk";

export interface ProviderToolCall {
  id: string;
  name: string;
  args: unknown;
}

export interface ProviderMessageContentPart {
  type: 'text' | 'image_url';
  text?: string;
  image_url?: {
    url: string;
    detail?: 'low' | 'high' | 'auto';
  };
}

export interface ProviderMessage {
  role: 'user' | 'assistant' | 'system' | 'tool';
  content?: string | ProviderMessageContentPart[];
  tool_calls?: ProviderToolCall[];
  tool_call_id?: string;
  reasoning?: string;
}

export interface TokenUsage {
  promptTokens: number;
  completionTokens: number;
  totalTokens: number;
  reasoningTokens?: number;
}

export type ProviderEvent =
  | { type: 'reasoning'; text: string }
  | { type: 'content'; text: string }
  | { type: 'tool_call'; call: ProviderToolCall }
  | { type: 'error'; error: string }
  | { type: 'usage'; usage: TokenUsage }
  | { type: 'request_sent'; request: Record<string, unknown> };

export interface ProviderTool {
  name: string;
  description: string;
  inputSchema: object;
}

export type ProviderType = 'openai' | 'anthropic' | 'custom';

export interface ProviderKeyConfig {
  envVar: string;
  supportsRotation: boolean;
  delimiter?: string;
}

export interface IProviderMetadata {
  id: string;
  type: ProviderType;
  requiresApiKey: boolean;
  keyConfig: ProviderKeyConfig;
  baseUrl?: string;
}

export interface ImageGenerationOptions {
  prompt: string;
  model?: string;
  n?: number;
  size?: '256x256' | '512x512' | '1024x1024';
  responseFormat?: 'b64_json' | 'url';
  imageDataUrl?: string;
  imageDataUrls?: string[];
  maskDataUrl?: string;
  strength?: number;
  guidanceScale?: number;
  numInferenceSteps?: number;
  seed?: number;
  kontextMaxMode?: boolean;
}

export interface ImageGenerationResult {
  images: Array<{ b64_json?: string; url?: string }>;
  cost?: number;
  paymentSource?: string;
  remainingBalance?: number;
}

export interface ProviderCompletionOptions {
  tools?: ProviderTool[];
  thinking?: boolean;
  reasoningEffort?: 'none' | 'minimal' | 'low' | 'medium' | 'high';
  max_tokens?: number;
  temperature?: number;
  model: string;
  signal?: AbortSignal;
}

export interface IProvider extends IProviderMetadata {
  stream(conversation: ProviderMessage[], options: ProviderCompletionOptions): AsyncIterable<StreamChunk>;
  complete(conversation: ProviderMessage[], options: ProviderCompletionOptions): Promise<ProviderMessage>;
  listModels(): ModelInfo[];
  getModelDetails?(modelId: string): ModelInfo | undefined;
  generateImage?(options: ImageGenerationOptions): Promise<ImageGenerationResult>;
}

export const createMessageWithStringContent = (role: ProviderMessage["role"], content: string) => ({
  role,
  content
});
