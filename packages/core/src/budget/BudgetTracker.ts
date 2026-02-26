import type { ModelInfo } from "@firmius/shared/types";
import { DEFAULT_BUDGET_CONFIG, type BudgetAllocation, type BudgetConfig, type BudgetState, type EvictionCandidate } from "./Types";

export class BudgetTracker {
  private config: BudgetConfig;
  private state: BudgetState;

  constructor(modelCtx: number, config: Partial<BudgetConfig> = {}) {
    this.config = { ...DEFAULT_BUDGET_CONFIG, ...config };
    this.state = {
      modelCtx,
      lastPromptTokens: 0,
      lastCompletionTokens: 0,
      systemUsage: 0,
      protectedUsage: 0,
      rollingUsage: 0,
      fileUsageChars: 0,
    };
  }

  setModelInfo(model: ModelInfo): void {
    this.state.modelCtx = model.ctx;
  }

  getAllocation(): BudgetAllocation {
    const ctx = this.state.modelCtx;
    return {
      fileBudget: Math.floor(ctx * this.config.fileBudgetPercent),
      protectedBudget: Math.floor(ctx * this.config.protectedBudgetPercent),
      rollingBudget: Math.floor(ctx * this.config.rollingBudgetPercent),
      reserveBudget: Math.floor(ctx * this.config.reserveBudgetPercent),
      systemBudget: Math.floor(ctx * this.config.systemBudgetPercent),
    };
  }

  recordUsage(promptTokens: number, completionTokens: number): void {
    this.state.lastPromptTokens = promptTokens;
    this.state.lastCompletionTokens = completionTokens;
  }

  updateCategoryUsage(category: 'system' | 'protected' | 'rolling', tokens: number): void {
    switch (category) {
      case 'system':
        this.state.systemUsage = tokens;
        break;
      case 'protected':
        this.state.protectedUsage = tokens;
        break;
      case 'rolling':
        this.state.rollingUsage = tokens;
        break;
    }
  }

  updateFileUsage(chars: number): void {
    this.state.fileUsageChars = chars;
  }

  getFileTokenUsage(): number {
    return Math.ceil(this.state.fileUsageChars / 4);
  }

  getTotalUsage(): number {
    return this.state.systemUsage + this.state.protectedUsage + this.state.rollingUsage;
  }

  getTotalWithFiles(): number {
    return this.getTotalUsage() + this.getFileTokenUsage();
  }

  getUsagePercent(): number {
    return (this.state.lastPromptTokens / this.state.modelCtx) * 100;
  }

  getRollingBudget(): number {
    return this.getAllocation().rollingBudget;
  }

  getFileBudget(): number {
    return this.getAllocation().fileBudget;
  }

  getRemainingForRolling(): number {
    const alloc = this.getAllocation();
    return Math.max(0, alloc.rollingBudget - this.state.rollingUsage);
  }

  getRemainingForFiles(): number {
    const alloc = this.getAllocation();
    const used = this.getFileTokenUsage();
    return Math.max(0, alloc.fileBudget - used);
  }

  needsEviction(): boolean {
    const alloc = this.getAllocation();
    return this.state.rollingUsage > alloc.rollingBudget * 0.9;
  }

  needsFileEviction(): boolean {
    const alloc = this.getAllocation();
    return this.getFileTokenUsage() > alloc.fileBudget;
  }

  getEvictionCandidates(
    entries: Array<{ turnIndex: number; tokenCount: number; priority: string; toolName?: string }>
  ): EvictionCandidate[] {
    const candidates: EvictionCandidate[] = entries
      .filter(e => e.priority !== 'critical')
      .map(e => ({
        type: 'tool_result' as const,
        turnIndex: e.turnIndex,
        tokenCount: e.tokenCount,
        priority: e.priority as 'high' | 'normal' | 'low',
      }))
      .sort((a, b) => {
        if (a.turnIndex !== b.turnIndex) return a.turnIndex - b.turnIndex;
        const priorityOrder = { low: 0, normal: 1, high: 2 };
        return priorityOrder[a.priority] - priorityOrder[b.priority];
      });
    return candidates;
  }

  getLastPromptTokens(): number {
    return this.state.lastPromptTokens;
  }

  getLastCompletionTokens(): number {
    return this.state.lastCompletionTokens;
  }

  getModelCtx(): number {
    return this.state.modelCtx;
  }

  getMaxRollingTurns(): number {
    return this.config.maxRollingTurns;
  }

  getState(): BudgetState {
    return { ...this.state };
  }

  canAddTokens(category: 'protected' | 'rolling', tokens: number): boolean {
    const alloc = this.getAllocation();
    if (category === 'protected') {
      return this.state.protectedUsage + tokens <= alloc.protectedBudget;
    }
    return this.state.rollingUsage + tokens <= alloc.rollingBudget;
  }

  canAddFileChars(chars: number): boolean {
    const alloc = this.getAllocation();
    const projectedTokens = Math.ceil((this.state.fileUsageChars + chars) / 4);
    return projectedTokens <= alloc.fileBudget;
  }
}
