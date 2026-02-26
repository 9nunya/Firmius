import { Engine, DEFAULT_PROVIDER } from "@firmius/core";
import type { IThread as Thread } from "@firmius/shared";
import type { Message } from "@firmius/shared/api";
import type { ProviderMessageContentPart } from "@firmius/shared";
import EventService from "./EventService";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";

const THREADS_DIR = join(homedir(), ".firmius", "threads");

export class StateService {
  async sendMessage(thread: Thread, content: string | ProviderMessageContentPart[]): Promise<Message> {
    try {
      const sequence = await thread.recordMessage(thread.leadAgent.id, {
        isUser: true,
        content,
        timestamp: Date.now(),
        tokens: 0,
      });

      if (sequence === 0 || !thread.title || thread.title === "New Thread") {
        this.generateThreadTitle(thread, typeof content === 'string' ? content : (content.find(p => p.type === 'text')?.text || "Multi-modal message")).catch(err => {
          console.error("Auto-title generation failed:", err);
        });
      }

      const message: Message = {
        sequence,
        isUser: true,
        content: content,
        timestamp: new Date(),
        tokens: 0,
        type: "response",
      };

      return message;
    } catch (error) {
      console.error("Failed to send message:", error);
      throw error;
    }
  }

  private async generateThreadTitle(thread: Thread, firstMessage: string): Promise<void> {
    try {
      if (!thread.leadAgent.context) return;
      const generationOptions = thread.leadAgent.context.execution.generationOptions;
      const providerId = generationOptions.providerId || DEFAULT_PROVIDER;
      const provider = Engine.providers[providerId];
      if (!provider) return;

      const modelId = generationOptions.modelId;

      const response = await provider.complete([
        {
          role: "user",
          content: `Generate a very brief (2-5 words) conversational title for a chat that starts with this message. Return ONLY the title text.
          
          Message: ${firstMessage.substring(0, 500)}`
        }
      ], {
        model: modelId,
        temperature: 0,
      });

      if (response.content) {
        const titleText = typeof response.content === 'string' ? response.content : (Array.isArray(response.content) ? response.content.map((p: any) => p.text || '').join('') : '');
        thread.title = titleText.replace(/^["']|["']$/g, '').trim();
        await (thread as any).checkpoint();
      }
    } catch (error) {
      console.error("Failed to generate title:", error);
    }
  }

  async getMessageHistory(thread: Thread): Promise<Message[]> {
    try {
      const messages: Message[] = [];

      // Get all agent IDs and read their journals for turns
      const agentIds = thread.getAllAgentIds ? thread.getAllAgentIds() : [];
      for (const agentId of agentIds) {
        try {
          const entries = thread.getAgentJournalEntries
            ? await thread.getAgentJournalEntries(agentId) as any[]
            : [];
          
          for (const entry of entries) {
            messages.push({
              sequence: entry.sequence,
              isUser: entry.type === "message" && entry.payload.isUser,
              content: entry.type === "message" ? entry.payload.content : (entry.payload.content || ""),
              timestamp: new Date(entry.timestamp),
              tokens: entry.type === "turn" ? entry.payload.tokens : 0,
              agentId: entry.agentId,
              type: entry.type === "message" ? "response" : (entry.payload.completed ? "response" : "monologue"),
              thinking: entry.type === "turn" ? entry.payload.reasoning : undefined,
              toolCalls: entry.type === "turn" ? entry.payload.toolCalls?.map((tc: any) => {
                const trWrapper = entry.payload.toolResults?.find((tr: any) => tr.id === tc.id);
                const result = trWrapper?.result;

                if (!result) return { ...tc, status: "running" };

                return {
                  name: tc.name,
                  callId: tc.id,
                  status: result.success ? "done" : "error",
                  summary: result.summary,
                  error: result.error,
                  metadata: result.metadata,
                  args: tc.args,
                  result: result.output,
                };
              }) : undefined,
            } as any);
          }
        } catch (err) {
          console.error(`Failed to read journal for agent ${agentId}:`, err);
        }
      }

      messages.sort((a, b) => {
        const aTime = a.timestamp.getTime();
        const bTime = b.timestamp.getTime();
        if (aTime !== bTime) return aTime - bTime;
        return a.sequence - b.sequence;
      });

      return messages;
    } catch (error) {
      console.error("Failed to get message history:", error);
      return [];
    }
  }

  async forgetLastTurn(thread: Thread): Promise<void> {
    try {
      await (thread as any).forgetLastTurn();
    } catch (error) {
      console.error("Failed to forget last turn:", error);
      throw error;
    }
  }

  async getThreadEvents(threadId: string): Promise<any[]> {
    try {
      return await EventService.getEventHistory(threadId);
    } catch (error) {
      console.error("Failed to get thread events:", error);
      return [];
    }
  }

  async loadThreadMetadata(threadId: string): Promise<any> {
    const path = join(THREADS_DIR, threadId, "thread-metadata.json");
    try {
      const content = await readFile(path, "utf8");
      return JSON.parse(content);
    } catch {
      return null;
    }
  }

  async listThreads(): Promise<any[]> {
    const threadIds: string[] = [];
    try {
      // Manual listing of directories in threads folder
      const { readdir } = await import("node:fs/promises");
      const entries = await readdir(THREADS_DIR, { withFileTypes: true });
      for (const entry of entries) {
        if (entry.isDirectory()) threadIds.push(entry.name);
      }
    } catch {
      return [];
    }

    const metadataPromises = threadIds.map(id => this.loadThreadMetadata(id));
    const metadatas = await Promise.all(metadataPromises);
    return metadatas.filter(m => m !== null);
  }

  async forgetEntry(thread: Thread, sequence: number): Promise<void> {
    await (thread as any).forgetEntry(sequence);
  }

  async unforgetEntry(thread: Thread, sequence: number): Promise<void> {
    await (thread as any).unforgetEntry(sequence);
  }

  async editMessage(thread: Thread, sequence: number, newContent: string): Promise<void> {
    await (thread as any).editUserMessage(sequence, newContent);
  }

  async forgetEventsAfterSequence(thread: Thread, sequence: number): Promise<void> {
    await (thread as any).forgetEventsAfterSequence(sequence);
  }

  async getEntry(thread: Thread, sequence: number): Promise<any> {
    const agentIds = thread.getAllAgentIds ? thread.getAllAgentIds() : [];
    for (const agentId of agentIds) {
      const entries = thread.getAgentJournalEntries ? await thread.getAgentJournalEntries(agentId) : [];
      const entry = entries.find((e: any) => e.sequence === sequence);
      if (entry) return entry;
    }
    return null;
  }
}

const stateService = new StateService();
export default stateService;
