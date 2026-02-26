import type { ProviderCompletionOptions, IProvider, ProviderMessage, TokenUsage, ModelInfo } from "@firmius/shared";
import type { StreamChunk } from "@firmius/shared/types/provider/StreamChunk";
import { logger } from "@firmius/shared";

const ZAI_CODING_PLAN_BASE_URL = "https://api.z.ai/api/coding/paas/v4"

// GLM models configuration with capabilities and modalities
const GLM_MODELS: Record<string, ModelInfo> = {
  "glm-4.7-flash": {
    id: "glm-4.7-flash",
    name: "glm-4.7-flash",
    ctx: 200000,
    maxOutputTokens: 128000,
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
  "glm-4.7-flashx": {
    id: "glm-4.7-flashx",
    name: "glm-4.7-flashx",
    ctx: 200000,
    maxOutputTokens: 128000,
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
  "glm-4.7": {
    id: "glm-4.7",
    name: "glm-4.7",
    ctx: 200000,
    maxOutputTokens: 128000,
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
      effortLevels: ["low", "medium", "high"]
    }
  },
  "glm-4.6": {
    id: "glm-4.6",
    name: "glm-4.6",
    ctx: 200000,
    maxOutputTokens: 128000,
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
  "glm-4.5": {
    id: "glm-4.5",
    name: "glm-4.5",
    ctx: 200000,
    maxOutputTokens: 128000,
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
  "glm-4.5-air": {
    id: "glm-4.5-air",
    name: "glm-4.5-air",
    ctx: 200000,
    maxOutputTokens: 128000,
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

export class ZaiProvider implements IProvider {
  id = "zai";
  type = "custom" as const;
  requiresApiKey = true;
  keyConfig = {
    envVar: "ZAI_API_KEY",
    supportsRotation: false
  };
  baseUrl = ZAI_CODING_PLAN_BASE_URL;

  constructor(private apiKey: string) { }

  listModels(): ModelInfo[] {
    return Object.values(GLM_MODELS);
  }

  getModelDetails(modelId: string): ModelInfo | undefined {
    return GLM_MODELS[modelId];
  }

  private formatMessages(messages: ProviderMessage[], options?: ProviderCompletionOptions): any[] {
    const modelInfo = options ? this.getModelDetails(options.model) : undefined;
    const supportsVision = modelInfo?.capabilities?.vision ?? false;

    return messages.map(m => {
      const msg: any = { role: m.role };
      if (typeof m.content === 'string') {
        msg.content = m.content;
      } else if (Array.isArray(m.content)) {
        if (supportsVision) {
          msg.content = m.content.map(p => {
            if (p.type === 'text') return { type: 'text', text: p.text };
            if (p.type === 'image_url') return { 
              type: 'image_url', 
              image_url: { 
                url: p.image_url?.url,
                detail: p.image_url?.detail || 'auto'
              } 
            };
            return p;
          });
        } else {
          // Handle content parts - extract text, ignore images (GLM doesn't support vision)
          const textParts = m.content
            .filter(p => p.type === 'text')
            .map(p => p.text)
            .join('');
          msg.content = textParts;
        }
      }
      if (m.tool_call_id) msg.tool_call_id = m.tool_call_id;
      if (m.tool_calls && m.tool_calls.length > 0) {
        msg.tool_calls = m.tool_calls.map(tc => ({
          id: tc.id,
          type: 'function',
          function: {
            name: tc.name,
            arguments: typeof tc.args === 'string' ? tc.args : JSON.stringify(tc.args)
          }
        }));
        if (msg.content === undefined) msg.content = "";
      }
      if (m.role === 'assistant' && (m as any).reasoning && !msg.tool_calls) {
        msg.reasoning_content = (m as any).reasoning;
      }
      return msg;
    });
  }

  async complete(conversation: ProviderMessage[], options: ProviderCompletionOptions): Promise<ProviderMessage> {
    const glmMessages = this.formatMessages(conversation, options);

    const request: any = {
      model: options.model,
      messages: glmMessages,
      stream: false,
      temperature: options.temperature ?? 0.7,
      tools: options.tools && options.tools.length > 0 ? options.tools.map(t => {
        const parameters = JSON.parse(JSON.stringify(t.inputSchema));
        delete parameters.$schema;
        delete parameters.additionalProperties;
        return {
          type: 'function',
          function: {
            name: t.name,
            description: t.description,
            parameters
          }
        };
      }) : undefined
    }

    const res = await fetch(`${ZAI_CODING_PLAN_BASE_URL}/chat/completions`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        "Authorization": `Bearer ${this.apiKey}`
      },
      body: JSON.stringify(request)
    });

    if (!res.ok) {
      throw new Error(`ZAI API ${res.status} error: ${await res.text()}`);
    }

    const data = await res.json() as any;
    const choice = data.choices[0];
    const message = choice.message;

    return {
      role: message.role,
      content: message.content,
      tool_calls: message.tool_calls ? message.tool_calls.map((tc: any) => {
        let args = '{}';
        try {
          args = tc.function.arguments || '{}';
        } catch (e) {
          logger.warn(`[ZaiProvider] Failed to parse tool call arguments: ${e instanceof Error ? e.message : e}`);
        }
        return {
          id: tc.id,
          name: tc.function.name,
          args
        };
      }) : undefined,
      reasoning: message.reasoning_content
    } as any;
  }

   async *stream(conversation: ProviderMessage[], options: ProviderCompletionOptions): AsyncIterable<StreamChunk> {
     const glmMessages = this.formatMessages(conversation, options);

     const request: any = {
       model: options.model,
       messages: glmMessages,
       stream: true,
       stream_options: { include_usage: true },
       temperature: options.temperature ?? 0.7,
       tools: options.tools && options.tools.length > 0 ? options.tools.map(t => {
         const parameters = JSON.parse(JSON.stringify(t.inputSchema));
         delete parameters.$schema;
         delete parameters.additionalProperties;

         return {
           type: 'function',
           function: {
             name: t.name,
             description: t.description,
             parameters
           }
         };
       }) : undefined
     }

      // Only enable thinking if model supports reasoning and explicitly requested
      const modelInfo = this.getModelDetails(options.model);
      if (options.thinking && modelInfo?.reasoning?.supported) {
        request.thinking = { type: "enabled" };
      }

      yield { type: "request_sent", request };

      let retries = 0;
     const maxRetries = 3;

     while (retries < maxRetries) {
       try {
         const res = await fetch(`${ZAI_CODING_PLAN_BASE_URL}/chat/completions`, {
           method: "POST",
           headers: {
             "Content-Type": "application/json",
             "Authorization": `Bearer ${this.apiKey}`
           },
           body: JSON.stringify(request),
           signal: options.signal
         });

         if (!res.ok) {
           const text = await res.text();
           if (res.status === 429 && retries < maxRetries - 1) {
             const delay = Math.pow(2, retries) * 2000;
             logger.warn(`[ZaiProvider] Rate limited (429). Retrying in ${delay}ms...`);
             await new Promise(r => setTimeout(r, delay));
             retries++;
             continue;
           }
           yield { type: "error", error: `ZAI API ${res.status} error: ${text}` };
           return;
         }

          const reader = res.body!.getReader();
          const decoder = new TextDecoder();
          let buffer = "";
          const toolCallBuffers: Map<number, { id: string; name: string; args: string }> = new Map();

         try {
           while (true) {
             const { done, value } = await reader.read();
             if (done) break;

             buffer += decoder.decode(value, { stream: true });
             const lines = buffer.split("\n");
             buffer = lines.pop() || "";

             for (const line of lines) {
               if (line.startsWith("data: ")) {
                 const dataStr = line.slice(6);
                 if (dataStr === '[DONE]') continue;

                 try {
                   const data = JSON.parse(dataStr);

                   if (data.usage) {
                     const usage: TokenUsage = {
                       promptTokens: data.usage.prompt_tokens,
                       completionTokens: data.usage.completion_tokens,
                       totalTokens: data.usage.total_tokens,
                       reasoningTokens: data.usage.completion_tokens_details?.reasoning_tokens
                     }

                      yield { type: "usage", tokens: usage.totalTokens, usage }
                   }

                   const delta = data.choices?.[0].delta;
                   if (!delta) continue;

                    if (delta.reasoning_content) yield { type: "reasoning", text: typeof delta.reasoning_content === 'string' ? delta.reasoning_content : JSON.stringify(delta.reasoning_content) }
                    if (delta.content) yield { type: "content", text: typeof delta.content === 'string' ? delta.content : JSON.stringify(delta.content) }
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
                        if (!buffer) continue;

                        if (tc.function?.name) {
                          buffer.name = tc.function.name;
                        }

                        if (tc.function?.arguments) {
                          buffer.args += tc.function.arguments;
                        }

                        const choice = data.choices?.[0];
                        if (choice?.finish_reason || buffer.args) {
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
                              logger.error(`[ZaiProvider] Failed to parse tool call args: ${buffer.args}, error: ${e}`);
                            }
                          }
                        }
                      }
                    }
                 } catch (e) { }
               }
             }
           }
         } finally {
           reader.releaseLock();
         }
         return;
       } catch (e: any) {
         if (e.name === 'AbortError') {
           return; // exit generator cleanly on abort
         }
         if (retries < maxRetries - 1) {
           retries++;
           await new Promise(r => setTimeout(r, 1000));
          continue;
        }
        yield { type: "error", error: e.message }
        return;
      }
    }
  }
}
