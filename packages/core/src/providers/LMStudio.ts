import type { ProviderCompletionOptions, IProvider, ProviderMessage, ModelInfo } from "@firmius/shared";
import type { StreamChunk } from "@firmius/shared/types/provider/StreamChunk";
import { logger } from "@firmius/shared";

const LM_STUDIO_API_BASE = process.env.LM_STUDIO_API_BASE || "http://localhost:1234/v1";

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
}

interface LMStudioModelResponse {
  id: string;
  object: string;
  created: number;
  owned_by: string;
  name?: string;
  context_length?: number;
  max_output_tokens?: number;
  capabilities?: {
    vision?: boolean;
    reasoning?: boolean;
    tool_calling?: boolean;
  };
}

interface LMStudioModelsResponse {
  object: string;
  data: LMStudioModelResponse[];
}

export class LMStudioProvider implements IProvider {
  id = "lmstudio";
  type = "openai" as const;
  requiresApiKey = false;
  keyConfig = {
    envVar: "LMSTUDIO_API_KEY",
    supportsRotation: false
  };
  baseUrl = LM_STUDIO_API_BASE;

  private cachedModels: ModelInfo[] | null = null;
  private modelsFetched: boolean = false;
  private apiKey: string;

  constructor(apiKey: string) {
    this.apiKey = apiKey;
  }

  private mapCapabilities(capabilities?: LMStudioModelResponse['capabilities']): { vision: boolean; reasoning: boolean; toolCalling: boolean; parallelToolCalls: boolean; structuredOutput: boolean; pdfUpload: boolean } {
    return {
      vision: capabilities?.vision ?? false,
      reasoning: capabilities?.reasoning ?? false,
      toolCalling: capabilities?.tool_calling ?? true,
      parallelToolCalls: capabilities?.tool_calling ?? true,
      structuredOutput: true,
      pdfUpload: false
    };
  }

  private async fetchModels(): Promise<ModelInfo[]> {
    try {
      const res = await fetch(`${LM_STUDIO_API_BASE}/models`, {
        method: "GET",
        headers: {
          "Authorization": `Bearer ${this.apiKey}`,
          "Content-Type": "application/json"
        }
      });

      if (!res.ok) {
        logger.warn(`[LMStudioProvider] Could not fetch models: ${res.status}`);
        return [];
      }

      const data = await res.json() as LMStudioModelsResponse;

      const models: ModelInfo[] = data.data.map(model => {
        const capabilities = this.mapCapabilities(model.capabilities);

        return {
          id: model.id,
          name: model.id,
          ctx: model.context_length ?? 32768,
          maxOutputTokens: model.max_output_tokens,
          capabilities,
          modalities: {
            input: capabilities.vision ? ["text", "image"] : ["text"],
            output: ["text"]
          },
          reasoning: {
            supported: capabilities.reasoning,
            effortLevels: capabilities.reasoning ? ["low", "medium", "high"] : undefined
          }
        };
      });

      this.cachedModels = models;
      this.modelsFetched = true;
      logger.info(`[LMStudioProvider] Cached ${models.length} models from API`);
      return models;
    } catch (error: any) {
      logger.error(`[LMStudioProvider] Failed to fetch models: ${error.message}`);
      return [];
    }
  }

  listModels(): ModelInfo[] {
    if (this.modelsFetched && this.cachedModels) {
      return this.cachedModels;
    }
    this.fetchModels().catch(() => {});
    return this.cachedModels ?? [];
  }

  getModelDetails(modelId: string): ModelInfo | undefined {
    if (!this.cachedModels) return undefined;
    return this.cachedModels.find(m => m.name === modelId);
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

  private formatMessages(messages: ProviderMessage[], options?: ProviderCompletionOptions): any[] {
    const modelInfo = options ? this.getModelDetails(options.model) : undefined;
    const supportsVision = modelInfo?.capabilities?.vision ?? false;

    return messages.map(m => {
      const msg: any = { role: m.role };

      if (typeof m.content === "string") {
        msg.content = m.content;
      } else if (Array.isArray(m.content)) {
        if (supportsVision) {
          msg.content = m.content.map((part: any) => {
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

  async *stream(conversation: ProviderMessage[], options: ProviderCompletionOptions): AsyncIterable<StreamChunk> {
    const requestBody: any = {
      model: options.model || "local-model",
      messages: this.formatMessages(conversation, options),
      stream: true
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

    yield { type: "request_sent", request: requestBody };

    try {
      const res = await fetch(`${LM_STUDIO_API_BASE}/chat/completions`, {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "Authorization": `Bearer ${this.apiKey}`
        },
        body: JSON.stringify(requestBody),
        signal: options.signal
      });

      if (!res.ok) {
        const errorText = await res.text();
        logger.error(`[LMStudio] API error ${res.status}: ${errorText}`);
        yield { type: "error", error: errorText };
        return;
      }

      const reader = res.body?.getReader();
      if (!reader) {
        logger.error("[LMStudio] No response body");
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

                  const buf = toolCallBuffers.get(index);

                  if (tc.function?.name) {
                    if (buf) {
                      buf.name = tc.function.name;
                    }
                  }

                  if (tc.function?.arguments) {
                    buf.args += tc.function.arguments;
                  }

                  if (choice.finish_reason || buf.args) {
                    try {
                      if (!buf.args || buf.args.trim().length === 0) {
                        throw new Error("Empty tool call arguments");
                      }

                      const openBraces = (buf.args.match(/\{/g) || []).length;
                      const closeBraces = (buf.args.match(/\}/g) || []).length;
                      const openBrackets = (buf.args.match(/\[/g) || []).length;
                      const closeBrackets = (buf.args.match(/\]/g) || []).length;

                      if (openBraces !== closeBraces || openBrackets !== closeBrackets) {
                        throw new Error("Unbalanced braces/brackets in tool call arguments");
                      }

                      const parsedArgs = JSON.parse(buf.args);

                      if (typeof parsedArgs !== 'object' || parsedArgs === null || Array.isArray(parsedArgs)) {
                        throw new Error("Tool call arguments must be an object");
                      }

                      yield {
                        type: "tool_call",
                        call: {
                          id: buf.id,
                          name: buf.name,
                          args: parsedArgs
                        }
                      };
                      toolCallBuffers.delete(index);
                    } catch (e) {
                      if (choice.finish_reason) {
                        logger.error(`[LMStudio] Failed to parse tool call args: ${buf.args}, error: ${e}`);
                        yield {
                          type: "tool_call",
                          call: {
                            id: buf.id,
                            name: buf.name,
                            args: {}
                          }
                        };
                        toolCallBuffers.delete(index);
                      } else {
                        logger.debug(`[LMStudio] Incomplete tool call args: ${buf.args}`);
                      }
                    }
                  }
                }
              }
            } catch (e) {
              logger.debug(`[LMStudio] Failed to parse chunk: ${data}`);
            }
          }
        }

        for (const event of processContentBuffer()) {
          yield event;
        }
      } finally {
        reader.releaseLock();
      }
    } catch (e: any) {
      if (e.name === 'AbortError') {
        return;
      }
      throw e;
    }
  }

  async complete(conversation: ProviderMessage[], options: ProviderCompletionOptions): Promise<ProviderMessage> {
    const requestBody: any = {
      model: options.model || "local-model",
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

    const res = await fetch(`${LM_STUDIO_API_BASE}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${this.apiKey}`
      },
      body: JSON.stringify(requestBody),
      signal: options.signal
    });

    if (!res.ok) {
      const errorText = await res.text();
      logger.error(`[LMStudio] API error ${res.status}: ${errorText}`);
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
        totalTokens: data.usage.total_tokens
      };
    }

    return message;
  }
}
