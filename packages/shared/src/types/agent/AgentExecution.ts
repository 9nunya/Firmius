export interface GenerationOptions {
  providerId: string;
  modelId: string;
  reasoningEffort?: 'none' | 'minimal' | 'low' | 'medium' | 'high';
  maxTokens?: number;
}

export interface DiscoveryConfig {
  topology: string;
  manifests: string;
  symbols: string;
}

export interface AgentExecution {
  generationOptions: GenerationOptions;
  maxContextChars: number;
  tags: Record<string, string>;
  disableCompaction: boolean;
  anchors: Set<string>;
  injectedContext?: string;
  discovery?: DiscoveryConfig;
}
