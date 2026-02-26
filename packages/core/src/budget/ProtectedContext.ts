import type { ProtectedEntry } from "./Types";

export class ProtectedContext {
  private objective: string = '';
  private anchors: Set<string> = new Set();
  private keyFacts: Map<string, string> = new Map();

  setObjective(objective: string): void {
    this.objective = objective;
  }

  getObjective(): string {
    return this.objective;
  }

  addAnchor(decision: string): void {
    this.anchors.add(decision);
  }

  removeAnchor(decision: string): void {
    this.anchors.delete(decision);
  }

  getAnchors(): string[] {
    return Array.from(this.anchors);
  }

  addKeyFact(key: string, value: string): void {
    this.keyFacts.set(key, value);
  }

  removeKeyFact(key: string): void {
    this.keyFacts.delete(key);
  }

  getKeyFacts(): Map<string, string> {
    return new Map(this.keyFacts);
  }

  getTokenCount(): number {
    let total = 0;
    total += Math.ceil(this.objective.length / 4);
    for (const anchor of this.anchors) {
      total += Math.ceil(anchor.length / 4);
    }
    for (const [key, value] of this.keyFacts) {
      total += Math.ceil((key.length + value.length) / 4);
    }
    return total;
  }

  toSystemSection(): string {
    const sections: string[] = [];
    
    if (this.anchors.size > 0) {
      sections.push(`## TASK ANCHORS (Immutable Decisions)
${Array.from(this.anchors).map(a => `- ${a}`).join('\n')}`);
    }
    
    if (this.keyFacts.size > 0) {
      sections.push(`## DISCOVERED CONTEXT
${Array.from(this.keyFacts).map(([k, v]) => `- **${k}**: ${v}`).join('\n')}`);
    }
    
    return sections.join('\n\n');
  }

  clear(): void {
    this.objective = '';
    this.anchors.clear();
    this.keyFacts.clear();
  }

  getState(): {
    objective: string;
    anchors: string[];
    recentUserMessages: ProtectedEntry[];
    keyFacts: Record<string, string>;
  } {
    return {
      objective: this.objective,
      anchors: Array.from(this.anchors),
      recentUserMessages: [],
      keyFacts: Object.fromEntries(this.keyFacts),
    };
  }

  restoreState(state: {
    objective: string;
    anchors: string[];
    recentUserMessages: ProtectedEntry[];
    keyFacts: Record<string, string>;
  }): void {
    this.objective = state.objective;
    this.anchors = new Set(state.anchors);
    this.keyFacts = new Map(Object.entries(state.keyFacts));
  }

  hasContent(): boolean {
    return !!this.objective || this.anchors.size > 0 || this.keyFacts.size > 0;
  }
}
