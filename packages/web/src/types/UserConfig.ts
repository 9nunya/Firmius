export interface PurposeDefaultModel {
  providerId: string;
  modelId: string;
}

export interface UserConfig {
  defaultModels: Record<string, PurposeDefaultModel>;
}
