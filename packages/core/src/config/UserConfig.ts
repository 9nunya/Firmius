export interface PurposeDefaultModel {
  providerId: string;
  modelId: string;
  reasoningEffort?: 'none' | 'minimal' | 'low' | 'medium' | 'high';
}

export interface UserConfig {
  defaultModels: Record<string, PurposeDefaultModel>;
}

export const DEFAULT_USER_CONFIG: UserConfig = {
  defaultModels: {},
};
