export interface StreamChunkReasoning {
  type: 'reasoning';
  text: string;
}

export interface StreamChunkContent {
  type: 'content';
  text: string;
}

export interface StreamChunkToolCall {
  type: 'tool_call';
  call: StreamChunkToolCallData;
}

export interface StreamChunkToolCallPreparing {
  type: "tool_call_preparing";
  call: {
    id: string;
    name: string;
  };
}

export interface StreamChunkToolCallData {
  id?: string;
  name: string;
  args: string | Record<string, unknown>;
}

export interface StreamChunkError {
  type: 'error';
  error: string;
}

export interface StreamChunkUsage {
  type: 'usage';
  tokens: number;
  usage?: {
    promptTokens: number;
    completionTokens: number;
    totalTokens: number;
    reasoningTokens?: number;
  };
}

export interface StreamChunkRequestSent {
  type: 'request_sent';
  request?: Record<string, unknown>;
}

export type StreamChunkData = 
  | StreamChunkReasoning
  | StreamChunkContent
  | StreamChunkToolCall
  | StreamChunkToolCallPreparing
  | StreamChunkError
  | StreamChunkUsage
  | StreamChunkRequestSent;

export type StreamChunk = StreamChunkData;
