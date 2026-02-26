import type {
  ProviderCompletionOptions,
  IProvider,
  ProviderMessage,
  TokenUsage,
  ImageGenerationOptions,
  ImageGenerationResult,
  ModelCapabilities,
  ModelModalities,
  ModelInfo
} from "@firmius/shared";
import type { StreamChunk } from "@firmius/shared/types/provider/StreamChunk";
import { logger } from "@firmius/shared";

// Toggle this to switch between model endpoints
// 'canonical' - All visible text models (respects user's "Also show paid models" preference)
// 'subscription' - Only subscription-included models
// 'paid' - Only paid/premium models
type ModelEndpointType = 'canonical' | 'subscription' | 'paid';
const MODEL_ENDPOINT_TYPE: ModelEndpointType = 'subscription';

const NANOGPT_API_BASE = "https://nano-gpt.com/api";

const MODEL_ENDPOINTS: Record<ModelEndpointType, string> = {
  canonical: "/v1/models",
  subscription: "/subscription/v1/models",
  paid: "/paid/v1/models"
};

// Rate limiting configuration with exponential backoff
const RATE_LIMIT_CONFIG = {
  maxRetriesPerKey: 3,
  baseDelay: 2000,
  maxDelay: 30000,
  keyRotationDelay: 1000
};

interface NanoGPTModelResponse {
  id: string;
  object: string;
  created: number;
  owned_by: string;
  name?: string;
  description?: string;
  context_length?: number;
  max_output_tokens?: number;
  capabilities?: {
    vision?: boolean;
    reasoning?: boolean;
    tool_calling?: boolean;
    parallel_tool_calls?: boolean;
    structured_output?: boolean;
    pdf_upload?: boolean;
  };
  pricing?: {
    prompt?: number;
    completion?: number;
    currency?: string;
    unit?: string;
  };
}

interface NanoGPTModelsResponse {
  object: string;
  data: NanoGPTModelResponse[];
}

interface OpenAIStreamChoice {
  index: number;
  delta: {
    role?: string;
    content?: string;
    reasoning?: string;
    reasoning_content?: string;
    tool_calls?: Array<{
      index: number;
      id?: string;
      type?: "function";
      function?: {
        name?: string;
        arguments?: string;
      };
    }>;
  };
  finish_reason: string | null;
}

interface OpenAIChatChunk {
  id: string;
  object: string;
  created: number;
  model: string;
  choices: OpenAIStreamChoice[];
  usage?: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
    completion_tokens_details?: {
      reasoning_tokens?: number;
    };
  };
}

export class NanoGPTProvider implements IProvider {
  id = "nanogpt";
  type = "openai" as const;
  requiresApiKey = true;
  keyConfig = {
    envVar: "NANOGPT_API_KEY",
    supportsRotation: true
  };
  baseUrl = NANOGPT_API_BASE;

  private apiKeys: string[] = [];
  private currentKeyIndex: number = 0;
  private exhaustedKeys: Set<number> = new Set();
  private cachedModels: ModelInfo[] | null = null;
  private modelsFetched: boolean = false;
  private modelsFetchPromise: Promise<ModelInfo[]> | null = null;
  private modelsCacheTimestamp: number = 0;
  private readonly MODELS_CACHE_TTL_MS = 5 * 60 * 1000; // 5 minutes

  constructor() {
    this.loadApiKeys();
  }

  private loadApiKeys(): void {
    // Check for numbered keys in environment
    if (typeof process !== 'undefined' && process.env) {
      for (let i = 1; i <= 10; i++) {
        const envVarName = `${this.keyConfig.envVar}_${i}`;
        const envKey = process.env[envVarName];
        if (typeof envKey === 'string' && envKey.trim().length > 0) {
          this.apiKeys.push(envKey.trim());
        }
      }
    }

    // If no numbered keys, check for single key or comma-separated
    if (this.apiKeys.length === 0) {
      const primaryKey = process.env[this.keyConfig.envVar];
      if (typeof primaryKey === 'string' && primaryKey.trim().length > 0) {
        // Check if comma-separated
        if (primaryKey.includes(',')) {
          this.apiKeys = primaryKey.split(',').map(k => k.trim()).filter(k => k.length > 0);
        } else {
          this.apiKeys.push(primaryKey.trim());
        }
      }
    }

    if (this.apiKeys.length === 0) {
      logger.warn(`[NanoGPTProvider] No API keys provided - provider will not be available`);
    } else {
      logger.info(`[NanoGPTProvider] Initialized with ${this.apiKeys.length} API key(s)`);
    }
  }

  private get currentApiKey(): string {
    const key = this.apiKeys[this.currentKeyIndex];
    if (!key) {
      throw new Error(`No API key available at index ${this.currentKeyIndex}`);
    }
    return key;
  }

  private rotateKey(): boolean {
    const startIndex = this.currentKeyIndex;

    do {
      this.currentKeyIndex = (this.currentKeyIndex + 1) % this.apiKeys.length;

      if (!this.exhaustedKeys.has(this.currentKeyIndex)) {
        logger.info(`[NanoGPTProvider] Rotated to key ${this.currentKeyIndex + 1}/${this.apiKeys.length}`);
        return true;
      }
    } while (this.currentKeyIndex !== startIndex);

    return false;
  }

  private markKeyExhausted(index: number): void {
    this.exhaustedKeys.add(index);
    logger.warn(`[NanoGPTProvider] Key ${index + 1}/${this.apiKeys.length} marked as exhausted`);
  }

  private async delay(ms: number, abortSignal?: AbortSignal): Promise<void> {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(resolve, ms);
      if (abortSignal) {
        abortSignal.addEventListener('abort', () => {
          clearTimeout(timeout);
          reject(new Error('Operation aborted'));
        });
      }
    });
  }

  private async fetchWithRetry(
    url: string,
    options: RequestInit,
    retryCount: number = 0,
    keyIndex: number = 0
  ): Promise<Response> {
    try {
      const res = await fetch(url, options);

      // Handle authentication errors - rotate key immediately
      if (res.status === 401) {
        this.markKeyExhausted(this.currentKeyIndex);

        if (this.rotateKey()) {
          logger.warn(`[NanoGPTProvider] Auth error (401). Rotating to next key...`);
          await this.delay(RATE_LIMIT_CONFIG.keyRotationDelay);
          const newOptions = {
            ...options,
            headers: {
              ...options.headers,
              "Authorization": `Bearer ${this.currentApiKey}`
            }
          };
          return this.fetchWithRetry(url, newOptions, 0, keyIndex + 1);
        } else {
          throw new Error("All API keys exhausted due to authentication errors");
        }
      }

      // Handle rate limiting with exponential backoff
      if (res.status === 429) {
        if (retryCount < RATE_LIMIT_CONFIG.maxRetriesPerKey) {
          const delayMs = Math.min(
            RATE_LIMIT_CONFIG.baseDelay * Math.pow(2, retryCount),
            RATE_LIMIT_CONFIG.maxDelay
          );
          logger.warn(`[NanoGPTProvider] Rate limited (429). Retrying in ${delayMs}ms... (attempt ${retryCount + 1}/${RATE_LIMIT_CONFIG.maxRetriesPerKey})`);
          try {
            await this.delay(delayMs, options.signal ?? undefined);
          } catch (e) {
            // Propagate abort without logging as error
            throw new Error(`Rate limit backoff interrupted: ${e instanceof Error ? e.message : String(e)}`);
          }
          return this.fetchWithRetry(url, options, retryCount + 1, keyIndex);
        } else {
          this.markKeyExhausted(this.currentKeyIndex);

          if (this.rotateKey()) {
            logger.warn(`[NanoGPTProvider] Max retries reached. Rotating to next key...`);
            try {
              await this.delay(RATE_LIMIT_CONFIG.keyRotationDelay, options.signal ?? undefined);
            } catch (e) {
              throw new Error(`Key rotation delay interrupted: ${e instanceof Error ? e.message : String(e)}`);
            }
            const newOptions = {
              ...options,
              headers: {
                ...options.headers,
                "Authorization": `Bearer ${this.currentApiKey}`
              }
            };
            return this.fetchWithRetry(url, newOptions, 0, keyIndex + 1);
          } else {
            throw new Error("All API keys exhausted due to rate limiting");
          }
        }
      }

      return res;
    } catch (error: any) {
      if (error.message?.includes("All API keys exhausted")) {
        throw error;
      }

       if (retryCount < RATE_LIMIT_CONFIG.maxRetriesPerKey) {
         const delayMs = Math.min(
           RATE_LIMIT_CONFIG.baseDelay * Math.pow(2, retryCount),
           RATE_LIMIT_CONFIG.maxDelay
         );
         logger.warn(`[NanoGPTProvider] Network error. Retrying in ${delayMs}ms... (attempt ${retryCount + 1}/${RATE_LIMIT_CONFIG.maxRetriesPerKey})`);
         try {
           await this.delay(delayMs, options.signal ?? undefined);
         } catch (e) {
           throw new Error(`Network retry delay interrupted: ${e instanceof Error ? e.message : String(e)}`);
         }
         return this.fetchWithRetry(url, options, retryCount + 1, keyIndex);
       }
       throw error;
    }
  }

  private mapCapabilities(capabilities?: NanoGPTModelResponse['capabilities']): ModelCapabilities {
    return {
      vision: capabilities?.vision ?? false,
      reasoning: capabilities?.reasoning ?? false,
      toolCalling: capabilities?.tool_calling ?? false,
      parallelToolCalls: capabilities?.parallel_tool_calls ?? false,
      structuredOutput: capabilities?.structured_output ?? false,
      pdfUpload: capabilities?.pdf_upload ?? false
    };
  }

  private mapModalities(capabilities: ModelCapabilities): ModelModalities {
    const input: Array<'text' | 'image' | 'pdf' | 'audio'> = ['text'];
    const output: Array<'text' | 'image' | 'audio'> = ['text'];

    if (capabilities.vision) {
      input.push('image');
    }
    if (capabilities.pdfUpload) {
      input.push('pdf');
    }

    // Note: NanoGPT API doesn't explicitly return audio capabilities in the models endpoint
    // but we can infer from model names if needed

    return { input, output };
  }

  private async fetchModels(): Promise<ModelInfo[]> {
    // Deduplicate concurrent requests - return existing promise if in-flight
    if (this.modelsFetchPromise) {
      logger.debug("[NanoGPTProvider] Reusing in-flight models fetch request");
      return this.modelsFetchPromise;
    }

    if (this.apiKeys.length === 0) {
      logger.error("[NanoGPTProvider] Cannot fetch models: No API key available");
      return [];
    }

    // Create the fetch promise
    this.modelsFetchPromise = this.doFetchModels();

    try {
      const result = await this.modelsFetchPromise;
      return result;
    } finally {
      // Clear the promise so future calls can retry if needed
      this.modelsFetchPromise = null;
    }
  }

  private async doFetchModels(): Promise<ModelInfo[]> {
    try {
      const endpoint = MODEL_ENDPOINTS[MODEL_ENDPOINT_TYPE];
      const url = `${NANOGPT_API_BASE}${endpoint}?detailed=true`;

      logger.info(`[NanoGPTProvider] Fetching models from ${MODEL_ENDPOINT_TYPE} endpoint: ${endpoint}`);

      const res = await this.fetchWithRetry(url, {
        method: "GET",
        headers: {
          "Authorization": `Bearer ${this.currentApiKey}`,
          "Content-Type": "application/json"
        }
      });

      if (!res.ok) {
        const errorText = await res.text();
        throw new Error(`Failed to fetch models: ${res.status} ${errorText}`);
      }

      const data = await res.json() as NanoGPTModelsResponse;

      const models: ModelInfo[] = data.data.map(model => {
        const capabilities = this.mapCapabilities(model.capabilities);
        const modalities = this.mapModalities(capabilities);

        return {
          id: model.id,
          name: model.id,
          ctx: model.context_length ?? 128000,
          maxOutputTokens: model.max_output_tokens,
          capabilities,
          modalities,
          reasoning: {
            supported: capabilities.reasoning,
            effortLevels: capabilities.reasoning ? ['none', 'minimal', 'low', 'medium', 'high'] : undefined
          }
        };
      });

      this.cachedModels = models;
      this.modelsFetched = true;
      this.modelsCacheTimestamp = Date.now();

      logger.info(`[NanoGPTProvider] Cached ${models.length} models from ${MODEL_ENDPOINT_TYPE} endpoint`);

      return models;
    } catch (error: any) {
      logger.error(`[NanoGPTProvider] Failed to fetch models: ${error.message}`);
      return [];
    }
  }

  listModels(): ModelInfo[] {
    // Check if cache is still valid (within TTL)
    const cacheAge = Date.now() - this.modelsCacheTimestamp;
    const isCacheValid = this.modelsFetched && this.cachedModels && cacheAge < this.MODELS_CACHE_TTL_MS;

    if (isCacheValid) {
      return this.cachedModels!;
    }

    // Trigger async fetch in background only if not already fetching
    if (this.apiKeys.length > 0 && !this.modelsFetchPromise) {
      this.fetchModels().catch(err => {
        logger.error(`[NanoGPTProvider] Background model fetch failed: ${err.message}`);
      });
    }

    // Return cached models even if stale, or empty array
    return this.cachedModels ?? [];
  }

  getModelDetails(modelId: string): ModelInfo | undefined {
    if (!this.cachedModels) {
      return undefined;
    }
    return this.cachedModels.find(m => m.name === modelId);
  }

  private formatMessages(messages: ProviderMessage[]): any[] {
    return messages.map(m => {
      const msg: any = { role: m.role };

      if (typeof m.content === "string") {
        msg.content = m.content;
      } else if (Array.isArray(m.content)) {
        // Handle content parts (text + images)
        msg.content = m.content.map(part => {
          if (part.type === 'text') {
            return { type: 'text', text: part.text };
          } else if (part.type === 'image_url') {
            return {
              type: 'image_url',
              image_url: {
                url: part.image_url!.url,
                detail: part.image_url!.detail ?? 'auto'
              }
            };
          }
          return part;
        });
      }

      if (m.tool_call_id) {
        msg.tool_call_id = m.tool_call_id;
      }

      if (m.tool_calls && m.tool_calls.length > 0) {
        msg.tool_calls = m.tool_calls.map(tc => ({
          id: tc.id,
          type: "function",
          function: {
            name: tc.name,
            arguments: typeof tc.args === "string" ? tc.args : JSON.stringify(tc.args)
          }
        }));
      }

      return msg;
    });
  }

  private createRequestBody(
    conversation: ProviderMessage[],
    options: ProviderCompletionOptions,
    stream: boolean
  ): any {
    const requestBody: any = {
      model: options.model,
      messages: this.formatMessages(conversation),
      stream
    };

    if (stream) {
      requestBody.stream_options = { include_usage: true };
    }

    if (options.temperature !== undefined) {
      requestBody.temperature = options.temperature;
    }

    if (options.max_tokens !== undefined) {
      requestBody.max_tokens = options.max_tokens;
    }

    if (options.reasoningEffort !== undefined) {
      requestBody.reasoning_effort = options.reasoningEffort;
    }

    if (options.tools && options.tools.length > 0) {
      requestBody.tools = options.tools.map(tool => {
        const parameters = JSON.parse(JSON.stringify(tool.inputSchema));
        delete parameters.$schema;
        delete parameters.additionalProperties;

        return {
          type: "function",
          function: {
            name: tool.name,
            description: tool.description,
            parameters
          }
        };
      });
    }

    return requestBody;
  }

  async *stream(
    conversation: ProviderMessage[],
    options: ProviderCompletionOptions
  ): AsyncIterable<StreamChunk> {
    if (this.apiKeys.length === 0) {
      yield { type: "error", error: "No API key available" };
      return;
    }

    // Reset exhausted keys for new request
    this.exhaustedKeys.clear();

    const requestBody = this.createRequestBody(conversation, options, true);

    yield { type: "request_sent", request: requestBody };

    try {
      const res = await this.fetchWithRetry(`${NANOGPT_API_BASE}/v1/chat/completions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "Authorization": `Bearer ${this.currentApiKey}`
        },
        body: JSON.stringify(requestBody),
        signal: options.signal
      });

      if (!res.ok) {
        const errorText = await res.text();
        logger.error(`[NanoGPTProvider] API error ${res.status}: ${errorText}`);
        yield { type: "error", error: errorText };
        return;
      }

      const reader = res.body?.getReader();
      if (!reader) {
        logger.error("[NanoGPTProvider] No response body");
        yield { type: "error", error: "No response body" };
        return;
      }

      const decoder = new TextDecoder();
      let buffer = "";
      const toolCallBuffers: Map<number, any> = new Map();

      try {
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;

          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split("\n");
          buffer = lines.pop() || "";

          for (const line of lines) {
            const trimmedLine = line.trim();
            if (!trimmedLine || !trimmedLine.startsWith("data: ")) continue;

            const data = trimmedLine.slice(6);
            if (data === "[DONE]") continue;

            try {
              const chunk: OpenAIChatChunk = JSON.parse(data);

              // Handle usage data
              if (chunk.usage) {
                const usage: TokenUsage = {
                  promptTokens: chunk.usage.prompt_tokens,
                  completionTokens: chunk.usage.completion_tokens,
                  totalTokens: chunk.usage.total_tokens,
                  reasoningTokens: chunk.usage.completion_tokens_details?.reasoning_tokens
                };
                yield { type: "usage", tokens: usage.totalTokens, usage };
              }

              if (!chunk.choices || chunk.choices.length === 0) continue;

              const choice = chunk.choices[0]!;
              const delta = choice.delta;

              if (!delta) continue;

              // Debug logging for kimi-k2.5:thinking
              if (options.model?.includes('kimi') && process.env.DEBUG_NANOGPT) {
                logger.debug(`[NanoGPTProvider] Delta keys: ${Object.keys(delta).join(', ')}, finish_reason: ${choice.finish_reason}`);
                if (delta.reasoning) logger.debug(`[NanoGPTProvider] Reasoning: ${delta.reasoning.substring(0, 50)}...`);
                if (delta.content) logger.debug(`[NanoGPTProvider] Content: ${delta.content.substring(0, 50)}...`);
                if (delta.tool_calls) logger.debug(`[NanoGPTProvider] Tool calls: ${JSON.stringify(delta.tool_calls)}`);
              }

              // Handle reasoning content (check both reasoning and reasoning_content)
              if (delta.reasoning) {
                yield { type: "reasoning", text: delta.reasoning };
              } else if (delta.reasoning_content) {
                yield { type: "reasoning", text: delta.reasoning_content };
              }

              // Handle regular content
              if (delta.content) {
                yield { type: "content", text: delta.content };
              }

              // Handle tool calls
              if (delta.tool_calls) {
                for (const tc of delta.tool_calls) {
                  const index = tc.index;

                  if (!toolCallBuffers.has(index)) {
                    toolCallBuffers.set(index, {
                      id: tc.id || `call_${index}`,
                      name: tc.function?.name || "",
                      args: ""
                    });
                  }

                  const buffer = toolCallBuffers.get(index);

                  if (tc.function?.name) {
                    buffer.name = tc.function.name;
                  }

                  if (tc.function?.arguments) {
                    buffer.args += tc.function.arguments;
                  }

                  // Try to parse complete tool calls
                  if (choice.finish_reason || buffer.args) {
                    try {
                      if (!buffer.args || buffer.args.trim().length === 0) {
                        throw new Error("Empty tool call arguments");
                      }

                      const openBraces = (buffer.args.match(/\{/g) || []).length;
                      const closeBraces = (buffer.args.match(/\}/g) || []).length;
                      const openBrackets = (buffer.args.match(/\[/g) || []).length;
                      const closeBrackets = (buffer.args.match(/\]/g) || []).length;

                      if (openBraces !== closeBraces || openBrackets !== closeBrackets) {
                        throw new Error("Unbalanced braces/brackets in tool call arguments");
                      }

                      const parsedArgs = JSON.parse(buffer.args);

                      if (typeof parsedArgs !== 'object' || parsedArgs === null || Array.isArray(parsedArgs)) {
                        throw new Error("Tool call arguments must be an object");
                      }

                      yield {
                        type: "tool_call",
                        call: {
                          id: buffer.id,
                          name: buffer.name,
                          args: parsedArgs
                        }
                      };
                      toolCallBuffers.delete(index);
                    } catch (e) {
                      if (choice.finish_reason) {
                        logger.error(`[NanoGPTProvider] Failed to parse tool call args: ${buffer.args}, error: ${e}`);
                        yield {
                          type: "tool_call",
                          call: {
                            id: buffer.id,
                            name: buffer.name,
                            args: {}
                          }
                        };
                        toolCallBuffers.delete(index);
                      } else {
                        logger.debug(`[NanoGPTProvider] Incomplete tool call args: ${buffer.args}`);
                      }
                    }
                  }
                }
              }
            } catch (e) {
              logger.debug(`[NanoGPTProvider] Failed to parse chunk: ${data}`);
            }
          }
        }
      } finally {
        reader.releaseLock();
      }
    } catch (error: any) {
      if (error.name === 'AbortError') return;
      logger.error(`[NanoGPTProvider] Stream error: ${error.message}`);
      yield { type: "error", error: error.message };
    }
  }

  async complete(
    conversation: ProviderMessage[],
    options: ProviderCompletionOptions
  ): Promise<ProviderMessage> {
    if (this.apiKeys.length === 0) {
      throw new Error("No API key available");
    }

    // Reset exhausted keys for new request
    this.exhaustedKeys.clear();

    const requestBody = this.createRequestBody(conversation, options, false);

    try {
      const res = await this.fetchWithRetry(`${NANOGPT_API_BASE}/v1/chat/completions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "Authorization": `Bearer ${this.currentApiKey}`
        },
        body: JSON.stringify(requestBody),
        signal: options.signal
      });

      if (!res.ok) {
        const errorText = await res.text();
        logger.error(`[NanoGPTProvider] API error ${res.status}: ${errorText}`);
        throw new Error(errorText);
      }

      const data: any = await res.json();
      const choice = data.choices?.[0];

      const message: ProviderMessage = {
        role: "assistant"
      };

      if (choice?.message?.content) {
        message.content = choice.message.content;
      }

      if (choice?.message?.reasoning || choice?.message?.reasoning_content) {
        (message as any).reasoning = choice.message.reasoning || choice.message.reasoning_content;
      }

      if (choice?.message?.tool_calls && choice.message.tool_calls.length > 0) {
        message.tool_calls = choice.message.tool_calls.map((tc: any) => ({
          id: tc.id,
          name: tc.function.name,
          args: typeof tc.function.arguments === "string" ? JSON.parse(tc.function.arguments) : tc.function.arguments
        }));
      }

      if (data.usage) {
        (message as any).usage = {
          promptTokens: data.usage.prompt_tokens,
          completionTokens: data.usage.completion_tokens,
          totalTokens: data.usage.total_tokens,
          reasoningTokens: data.usage.completion_tokens_details?.reasoning_tokens
        };
      }

      return message;
    } catch (error: any) {
      logger.error(`[NanoGPTProvider] Complete error: ${error.message}`);
      throw error;
    }
  }

  async generateImage(options: ImageGenerationOptions): Promise<ImageGenerationResult> {
    if (this.apiKeys.length === 0) {
      throw new Error("No API key available");
    }

    const requestBody: any = {
      prompt: options.prompt,
      model: options.model ?? "hidream",
      n: options.n ?? 1,
      size: options.size ?? "1024x1024",
      response_format: options.responseFormat ?? "b64_json"
    };

    if (options.imageDataUrl) {
      requestBody.imageDataUrl = options.imageDataUrl;
    }

    if (options.imageDataUrls) {
      requestBody.imageDataUrls = options.imageDataUrls;
    }

    if (options.maskDataUrl) {
      requestBody.maskDataUrl = options.maskDataUrl;
    }

    if (options.strength !== undefined) {
      requestBody.strength = options.strength;
    }

    if (options.guidanceScale !== undefined) {
      requestBody.guidance_scale = options.guidanceScale;
    }

    if (options.numInferenceSteps !== undefined) {
      requestBody.num_inference_steps = options.numInferenceSteps;
    }

    if (options.seed !== undefined) {
      requestBody.seed = options.seed;
    }

    if (options.kontextMaxMode !== undefined) {
      requestBody.kontext_max_mode = options.kontextMaxMode;
    }

     try {
       // Debug logging for request parameters
       if (process.env.DEBUG_NANOGPT) {
         logger.debug(`[NanoGPTProvider] Request: model=${requestBody.model}, max_tokens=${requestBody.max_tokens}, reasoning_effort=${requestBody.reasoning_effort}, tools=${requestBody.tools?.length}, stream_options=${JSON.stringify(requestBody.stream_options)}`);
         if (requestBody.messages && requestBody.messages.length > 0) {
           const lastMsg = requestBody.messages[requestBody.messages.length - 1];
           logger.debug(`[NanoGPTProvider] Last message role: ${lastMsg.role}, hasToolCalls: ${!!lastMsg.tool_calls}, content preview: ${typeof lastMsg.content === 'string' ? lastMsg.content.substring(0, 100) : '[complex]'}`);
         }
       }

       const res = await this.fetchWithRetry(`${NANOGPT_API_BASE}/v1/chat/completions`, {
         method: "POST",
         headers: {
           "Content-Type": "application/json",
           "Authorization": `Bearer ${this.currentApiKey}`
         },
         body: JSON.stringify(requestBody)
       });

      if (!res.ok) {
        const errorText = await res.text();
        throw new Error(`Image generation failed: ${res.status} ${errorText}`);
      }

      const data: any = await res.json();

      return {
        images: data.data.map((img: any) => ({
          b64_json: img.b64_json,
          url: img.url
        })),
        cost: data.cost,
        paymentSource: data.paymentSource,
        remainingBalance: data.remainingBalance
      };
    } catch (error: any) {
      logger.error(`[NanoGPTProvider] Image generation error: ${error.message}`);
      throw error;
    }
  }
}
