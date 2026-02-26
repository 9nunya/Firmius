import { randomUUID } from "node:crypto";
import type { IProvider, ProviderMessage } from "@firmius/shared/types";
import { type CheckpointData, type CompactionJob, type BufferEntry } from "./Types";
import { ContextCheckpoint } from "./ContextCheckpoint";

const SUMMARIZATION_PROMPT = `You are a context compaction agent. Your job is to summarize tool results and extract key information.

You will receive a list of tool results from previous turns. Your task is to:
1. Create a concise summary of what was done (2-3 paragraphs max)
2. Extract key facts discovered (important for continuity)
3. Note any decisions that were made

Respond in this exact JSON format:
{
  "summary": "Your summary here...",
  "preservedFacts": {
    "fact_key": "fact value",
    ...
  },
  "preservedDecisions": [
    "Decision 1",
    "Decision 2"
  ]
}`;

export class ParallelCompactor {
  private queue: CompactionJob[] = [];
  private active: CompactionJob | null = null;
  private provider: IProvider | null = null;
  private modelId: string | null = null;
  private completedCheckpoints: CheckpointData[] = [];
  private journalDir: string | null = null;
  private sequenceCounter: number = 0;

  setProvider(provider: IProvider, modelId: string): void {
    this.provider = provider;
    this.modelId = modelId;
  }

  setJournalDir(dir: string): void {
    this.journalDir = dir;
  }

  setSequenceCounter(counter: number): void {
    this.sequenceCounter = counter;
  }

  queueCompaction(entries: BufferEntry[], threadId: string, agentId: string): string {
    const jobId = randomUUID();
    
    const job: CompactionJob = {
      id: jobId,
      entries,
      status: 'pending',
    };

    this.queue.push(job);
    this.processQueue(threadId, agentId);
    
    return jobId;
  }

  private async processQueue(threadId: string, agentId: string): Promise<void> {
    if (this.active || this.queue.length === 0) return;
    if (!this.provider || !this.modelId) return;

    this.active = this.queue.shift()!;
    this.active.status = 'running';

    try {
      const checkpoint = await this.runSummarization(
        this.active.entries,
        threadId,
        agentId
      );

      this.active.result = checkpoint;
      this.active.status = 'complete';
      this.completedCheckpoints.push(checkpoint);

      if (this.journalDir) {
        await ContextCheckpoint.save(checkpoint, this.journalDir);
      }
    } catch (error) {
      this.active.status = 'failed';
      this.active.error = error instanceof Error ? error.message : String(error);
    }

    this.active = null;

    if (this.queue.length > 0) {
      setImmediate(() => this.processQueue(threadId, agentId));
    }
  }

  private async runSummarization(
    entries: BufferEntry[],
    threadId: string,
    agentId: string
  ): Promise<CheckpointData> {
    if (!this.provider || !this.modelId) {
      throw new Error('Provider or model not set');
    }

    const entryContent = entries.map(e => 
      `[Turn ${e.turnIndex}] ${e.toolName}:\n${e.content.substring(0, 1000)}${e.content.length > 1000 ? '...' : ''}`
    ).join('\n\n---\n\n');

    const messages: ProviderMessage[] = [
      { role: 'system', content: SUMMARIZATION_PROMPT },
      { role: 'user', content: `Please summarize the following tool results:\n\n${entryContent}` }
    ];

    let fullResponse = '';
    
    for await (const event of this.provider.stream(messages, {
      model: this.modelId,
      max_tokens: 2000,
    })) {
      if (event.type === 'content') {
        fullResponse += event.text;
      }
    }

    const parsed = this.parseSummarizationResponse(fullResponse);
    
    this.sequenceCounter++;
    
    return ContextCheckpoint.create(
      this.sequenceCounter,
      threadId,
      agentId,
      entries,
      parsed.summary,
      parsed.preservedFacts,
      parsed.preservedDecisions
    );
  }

  private parseSummarizationResponse(response: string): {
    summary: string;
    preservedFacts: Record<string, string>;
    preservedDecisions: string[];
  } {
    try {
      const jsonMatch = response.match(/\{[\s\S]*\}/);
      if (jsonMatch) {
        const parsed = JSON.parse(jsonMatch[0]);
        return {
          summary: parsed.summary || 'No summary generated.',
          preservedFacts: parsed.preservedFacts || {},
          preservedDecisions: parsed.preservedDecisions || [],
        };
      }
    } catch {
      // Fall through to default
    }

    return {
      summary: response.substring(0, 500) || 'No summary generated.',
      preservedFacts: {},
      preservedDecisions: [],
    };
  }

  getStatus(jobId: string): CompactionJob['status'] | null {
    const job = this.queue.find(j => j.id === jobId) || 
                (this.active?.id === jobId ? this.active : null);
    return job?.status ?? null;
  }

  getJob(jobId: string): CompactionJob | null {
    return this.queue.find(j => j.id === jobId) || 
           (this.active?.id === jobId ? this.active : null);
  }

  getCompletedCheckpoints(): CheckpointData[] {
    return [...this.completedCheckpoints];
  }

  getQueueLength(): number {
    return this.queue.length;
  }

  isProcessing(): boolean {
    return this.active !== null;
  }

  clearCompleted(): void {
    this.completedCheckpoints = [];
  }

  getState(): {
    queueLength: number;
    isProcessing: boolean;
    completedCount: number;
    sequenceCounter: number;
  } {
    return {
      queueLength: this.queue.length,
      isProcessing: this.isProcessing(),
      completedCount: this.completedCheckpoints.length,
      sequenceCounter: this.sequenceCounter,
    };
  }

  restoreState(checkpoints: CheckpointData[], sequenceCounter: number): void {
    this.completedCheckpoints = checkpoints;
    this.sequenceCounter = sequenceCounter;
  }

  async loadCheckpointsFromDisk(journalDir: string): Promise<void> {
    const checkpoints = await ContextCheckpoint.loadAll(journalDir);
    this.completedCheckpoints = checkpoints;
    if (checkpoints.length > 0) {
      const maxSequence = Math.max(...checkpoints.map(cp => cp.sequence));
      this.sequenceCounter = maxSequence;
    }
  }
}
