import type { ProviderCompletionOptions, IProvider, ProviderMessage, TokenUsage, ModelInfo } from "@firmius/shared";
import type { StreamChunk } from "@firmius/shared/types/provider/StreamChunk";
import { logger } from "@firmius/shared";

const ZEN_API_BASE = "https://opencode.ai/zen/v1";

// Rate limiting configuration
const RATE_LIMIT_CONFIG = {
  maxRetriesPerKey: 3,
  baseDelay: 2000,
  maxDelay: 30000,
  keyRotationDelay: 1000
};

interface OpenAIStreamChoice {
  index: number;
  delta: {
    role?: string;
    content?: string;
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

// FREE models with their context windows, endpoints, and capabilities
const FREE_MODELS: Record<string, ModelInfo & { endpoint: string; apiType: 'openai' | 'anthropic' }> = {
  "minimax-m2.1-free": {
    id: "minimax-m2.1-free",
    name: "minimax-m2.1-free",
    ctx: 204800,
    endpoint: "/messages",
    apiType: "anthropic",
    maxOutputTokens: 8192,
    capabilities: {
      vision: false,
      reasoning: false,
      toolCalling: true,
      parallelToolCalls: true,
      structuredOutput: true,
      pdfUpload: false
    },
    modalities: {
      input: ["text"],
      output: ["text"]
    }
  },
  "glm-4.7-free": {
    id: "glm-4.7-free",
    name: "glm-4.7-free",
    ctx: 200000,
    endpoint: "/chat/completions",
    apiType: "openai",
    maxOutputTokens: 8192,
    capabilities: {
      vision: false,
      reasoning: false,
      toolCalling: true,
      parallelToolCalls: true,
      structuredOutput: true,
      pdfUpload: false
    },
    modalities: {
      input: ["text"],
      output: ["text"]
    }
  },
  "kimi-k2.5-free": {
    id: "kimi-k2.5-free",
    name: "kimi-k2.5-free",
    ctx: 256000,
    endpoint: "/chat/completions",
    apiType: "openai",
    maxOutputTokens: 65000,
    capabilities: {
      vision: true,
      reasoning: true,
      toolCalling: true,
      parallelToolCalls: true,
      structuredOutput: true,
      pdfUpload: false
    },
    modalities: {
      input: ["text", "image"],
      output: ["text"]
    },
    reasoning: {
      supported: true,
      effortLevels: ["none", "minimal", "low", "medium", "high"]
    }
  },
  "big-pickle": {
    id: "big-pickle",
    name: "big-pickle",
    ctx: 200000,
    endpoint: "/chat/completions",
    apiType: "openai",
    maxOutputTokens: 8192,
    capabilities: {
      vision: false,
      reasoning: false,
      toolCalling: true,
      parallelToolCalls: true,
      structuredOutput: true,
      pdfUpload: false
    },
    modalities: {
      input: ["text"],
      output: ["text"]
    }
  },
  "gpt-5-nano": {
    id: "gpt-5-nano",
    name: "gpt-5-nano",
    ctx: 400000,
    endpoint: "/chat/completions",
    apiType: "openai",
    maxOutputTokens: 16384,
    capabilities: {
      vision: false,
      reasoning: false,
      toolCalling: true,
      parallelToolCalls: true,
      structuredOutput: true,
      pdfUpload: false
    },
    modalities: {
      input: ["text"],
      output: ["text"]
    }
  }
};

export class ZenProvider implements IProvider {
  id = "opencode";
  type = "custom" as const;
  requiresApiKey = true;
  keyConfig = {
    envVar: "ZEN_API_KEY",
    supportsRotation: true
  };
  baseUrl = ZEN_API_BASE;

  private apiKeys: string[];
  private currentKeyIndex: number = 0;
  private exhaustedKeys: Set<number> = new Set();

  constructor(apiKey: string | string[]) {
    if (typeof apiKey === 'string') {
      this.apiKeys = this.loadApiKeys(apiKey);
    } else {
      this.apiKeys = apiKey.filter(k => k && k.trim().length > 0);
    }

    if (this.apiKeys.length === 0) {
      logger.warn(`[ZenProvider] No API keys provided - provider will not be available`);
    } else {
      logger.info(`[ZenProvider] Initialized with ${this.apiKeys.length} API key(s)`);
    }
  }

  private loadApiKeys(primaryKey: string): string[] {
    const keys: string[] = [];

    if (typeof process !== 'undefined' && process.env) {
      for (let i = 1; i <= 10; i++) {
        const envVarName = `ZEN_API_KEY_${i}`;
        const envKey = process.env[envVarName];
        if (typeof envKey === 'string' && envKey.trim().length > 0) {
          keys.push(envKey.trim());
        }
      }
    }

    if (keys.length === 0 && primaryKey && primaryKey.trim().length > 0) {
      keys.push(primaryKey.trim());
    }

    return keys;
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
        logger.info(`[ZenProvider] Rotated to key ${this.currentKeyIndex + 1}/${this.apiKeys.length}`);
        return true;
      }
    } while (this.currentKeyIndex !== startIndex);

    return false;
  }

  private markKeyExhausted(index: number): void {
    this.exhaustedKeys.add(index);
    logger.warn(`[ZenProvider] Key ${index + 1}/${this.apiKeys.length} marked as exhausted`);
  }

  listModels(): ModelInfo[] {
    return Object.values(FREE_MODELS).map(({ endpoint, apiType, ...model }) => model);
  }

  getModelDetails(modelId: string): ModelInfo | undefined {
    const model = FREE_MODELS[modelId];
    if (!model) return undefined;
    const { endpoint, apiType, ...details } = model;
    return details;
  }

  private async delay(ms: number): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, ms));
  }

  private async fetchWithRetry(
    url: string,
    options: RequestInit,
    retryCount: number = 0,
    keyIndex: number = 0
  ): Promise<Response> {
    try {
      const res = await fetch(url, options);

      if (res.status === 401) {
        this.markKeyExhausted(this.currentKeyIndex);

        if (this.rotateKey()) {
          logger.warn(`[ZenProvider] Auth error (401). Rotating to next key...`);
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

      if (res.status === 429) {
        if (retryCount < RATE_LIMIT_CONFIG.maxRetriesPerKey) {
          const delay = Math.min(
            RATE_LIMIT_CONFIG.baseDelay * Math.pow(2, retryCount),
            RATE_LIMIT_CONFIG.maxDelay
          );
          logger.warn(`[ZenProvider] Rate limited (429). Retrying in ${delay}ms... (attempt ${retryCount + 1}/${RATE_LIMIT_CONFIG.maxRetriesPerKey})`);
          await this.delay(delay);
          return this.fetchWithRetry(url, options, retryCount + 1, keyIndex);
        } else {
          this.markKeyExhausted(this.currentKeyIndex);

          if (this.rotateKey()) {
            logger.warn(`[ZenProvider] Max retries reached. Rotating to next key...`);
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
        const delay = Math.min(
          RATE_LIMIT_CONFIG.baseDelay * Math.pow(2, retryCount),
          RATE_LIMIT_CONFIG.maxDelay
        );
        logger.warn(`[ZenProvider] Network error. Retrying in ${delay}ms... (attempt ${retryCount + 1}/${RATE_LIMIT_CONFIG.maxRetriesPerKey})`);
        await this.delay(delay);
        return this.fetchWithRetry(url, options, retryCount + 1, keyIndex);
      }
      throw error;
    }
  }

  private formatMessages(messages: ProviderMessage[], options?: ProviderCompletionOptions): any[] {
    const modelConfig = options ? FREE_MODELS[options.model] : undefined;
    const supportsVision = modelConfig?.capabilities?.vision ?? false;

    return messages
      .filter(m => {
        if (m.role === "assistant" && (!m.tool_calls || m.tool_calls.length === 0)) {
          return (typeof m.content === "string" && m.content.length > 0) || (Array.isArray(m.content) && m.content.length > 0);
        }
        return true;
      })
      .map(m => {
        const msg: any = { role: m.role };

        if (typeof m.content === "string") {
          msg.content = m.content;
        } else if (Array.isArray(m.content)) {
          if (supportsVision) {
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
          } else {
            const textParts = m.content.filter((p: any) => p.type === "text").map((p: any) => p.text);
            msg.content = textParts.join("");
          }
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

  private extractThinkingBlocks(content: string): { thinking: string; content: string } {
    const thinkingMatch = content.match(/<think>([\s\S]*?)<\/think>/);
    if (thinkingMatch && thinkingMatch[1]) {
      const thinking = thinkingMatch[1].trim();
      const cleanContent = content.replace(/<think>[\s\S]*?<\/think>/, "").trim();
      return { thinking, content: cleanContent };
    }
    return { thinking: "", content };
  }

  async *stream(conversation: ProviderMessage[], options: ProviderCompletionOptions): AsyncIterable<StreamChunk> {
    const modelConfig = FREE_MODELS[options.model];
    if (!modelConfig) {
      yield { type: "error", error: `Unknown model: ${options.model}. Available free models: ${Object.keys(FREE_MODELS).join(', ')}` };
      return;
    }

    const formattedMessages = this.formatMessages(conversation, options);

    const requestBody: any = {
      model: options.model,
      messages: formattedMessages,
      stream: true,
      stream_options: { include_usage: true }
    };

    if (options.temperature !== undefined) {
      requestBody.temperature = options.temperature;
    }

    if (options.max_tokens !== undefined) {
      requestBody.max_tokens = options.max_tokens;
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

    this.exhaustedKeys.clear();

    yield { type: "request_sent", request: requestBody };

    try {
      const res = await this.fetchWithRetry(`${ZEN_API_BASE}${modelConfig.endpoint}`, {
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
        logger.error(`[ZenProvider] API error ${res.status}: ${errorText}`);
        yield { type: "error", error: errorText };
        return;
      }

      const reader = res.body?.getReader();
      if (!reader) {
        logger.error("[ZenProvider] No response body");
        yield { type: "error", error: "No response body" };
        return;
      }

      const decoder = new TextDecoder();
      let buffer = "";
      const toolCallBuffers: Map<number, any> = new Map();
      let contentBuffer = "";
      let inThinkingBlock = false;

      function* processContentBuffer(): Generator<StreamChunk> {
        while (contentBuffer.length > 0) {
          if (inThinkingBlock) {
            const endIdx = contentBuffer.indexOf("</think>");
            if (endIdx === -1) {
              yield { type: "reasoning", text: contentBuffer };
              contentBuffer = "";
            } else {
              yield { type: "reasoning", text: contentBuffer.slice(0, endIdx) };
              contentBuffer = contentBuffer.slice(endIdx + 8);
              inThinkingBlock = false;
            }
          } else {
            const startIdx = contentBuffer.indexOf("<think>");
            if (startIdx === -1) {
              yield { type: "content", text: contentBuffer };
              contentBuffer = "";
            } else {
              if (startIdx > 0) {
                yield { type: "content", text: contentBuffer.slice(0, startIdx) };
              }
              contentBuffer = contentBuffer.slice(startIdx + 7);
              inThinkingBlock = true;
            }
          }
        }
      }

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

              if (delta.reasoning_content) {
                yield { type: "reasoning", text: delta.reasoning_content };
              }

              if (delta.content) {
                contentBuffer += delta.content;
                for (const event of processContentBuffer()) {
                  yield event;
                }
              }

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
                    if (buffer) {
                      buffer.name = tc.function.name;
                    }
                  }

                  if (tc.function?.arguments) {
                    buffer.args += tc.function.arguments;
                  }

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
                        logger.error(`[ZenProvider] Failed to parse tool call args: ${buffer.args}, error: ${e}`);
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
                        logger.debug(`[ZenProvider] Incomplete tool call args: ${buffer.args}`);
                      }
                    }
                  }
                }
              }

            } catch (e) {
              logger.debug(`[ZenProvider] Failed to parse chunk: ${data}`);
            }
          }
        }

        for (const event of processContentBuffer()) {
          yield event;
        }
      } finally {
        reader.releaseLock();
      }
    } catch (error: any) {
      if (error.name === 'AbortError') return;
      logger.error(`[ZenProvider] Stream error: ${error.message}`);
      yield { type: "error", error: error.message };
    }
  }

  async complete(conversation: ProviderMessage[], options: ProviderCompletionOptions): Promise<ProviderMessage> {
    const modelConfig = FREE_MODELS[options.model];
    if (!modelConfig) {
      throw new Error(`Unknown model: ${options.model}. Available free models: ${Object.keys(FREE_MODELS).join(', ')}`);
    }

    const requestBody: any = {
      model: options.model,
      messages: this.formatMessages(conversation, options),
      stream: false
    };

    if (options.temperature !== undefined) {
      requestBody.temperature = options.temperature;
    }

    if (options.max_tokens !== undefined) {
      requestBody.max_tokens = options.max_tokens;
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

    this.exhaustedKeys.clear();

    try {
      const res = await this.fetchWithRetry(`${ZEN_API_BASE}${modelConfig.endpoint}`, {
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
        logger.error(`[ZenProvider] API error ${res.status}: ${errorText}`);
        throw new Error(errorText);
      }

      const data: any = await res.json();
      const choice = data.choices?.[0];

      const message: ProviderMessage = {
        role: "assistant"
      };

      if (choice?.message?.content) {
        const { thinking, content } = this.extractThinkingBlocks(choice.message.content);
        if (thinking) {
          (message as any).reasoning = thinking;
        }
        message.content = content;
      }

      if (choice?.message?.reasoning_content) {
        (message as any).reasoning = choice.message.reasoning_content;
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
      logger.error(`[ZenProvider] Complete error: ${error.message}`);
      throw error;
    }
  }
}
