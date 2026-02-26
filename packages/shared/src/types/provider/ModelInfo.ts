export interface ModelCapabilities {
  vision: boolean;
  reasoning: boolean;
  toolCalling: boolean;
  parallelToolCalls: boolean;
  structuredOutput: boolean;
  pdfUpload: boolean;
}

export interface ModelModalities {
  input: Array<'text' | 'image' | 'pdf' | 'audio'>;
  output: Array<'text' | 'image' | 'audio'>;
}

export interface ModelInfo {
  id: string;
  name: string;
  ctx: number;
  maxOutputTokens?: number;
  capabilities: ModelCapabilities;
  modalities: ModelModalities;
  reasoning?: {
    supported: boolean;
    effortLevels?: string[];
  };
}
