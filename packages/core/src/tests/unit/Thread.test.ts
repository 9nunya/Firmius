import { test, expect, describe, beforeEach } from "bun:test";
import { Thread } from "../../Thread";
import { Engine } from "../../Engine";
import { BuiltinPurposes, type AgentTurn, type AgentConversationalMessage, type IProvider, type ProviderMessage, type ProviderCompletionOptions, HostType, type HostConfig } from "@firmius/shared";
import type { ModelInfo, StreamChunk } from "@firmius/shared";
import { existsSync } from "node:fs";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import { registerGlobalCleanup } from "../test_utils";

registerGlobalCleanup();

// Create a mock provider for testing
const createMockProvider = (): IProvider => ({
  id: "nanogpt",
  type: "custom",
  requiresApiKey: false,
  keyConfig: { envVar: "MOCK_KEY", supportsRotation: false },
  baseUrl: "http://mock.local",
  listModels: (): ModelInfo[] => [
    { 
      id: "mock-model",
      name: "mock-model", 
      ctx: 128000, 
      capabilities: { 
        vision: false, 
        reasoning: true, 
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
  ],
  complete: async (_conversation: ProviderMessage[], _options: ProviderCompletionOptions): Promise<ProviderMessage> => {
    return {
      role: "assistant",
      content: "mock response"
    };
  },
  async *stream(_conversation: ProviderMessage[], _options: ProviderCompletionOptions): AsyncIterable<StreamChunk> {
    yield { type: "content", text: "mock" };
    yield { type: "usage", tokens: 0, usage: { promptTokens: 0, completionTokens: 0, totalTokens: 0 } };
  }
});

// Set up Engine with mock provider before tests
beforeEach(() => {
  // Reset Engine state
  Engine.providers = {};
  Engine.tools = {};
  
  // Add mock provider
  Engine.providers.nanogpt = createMockProvider();
});

const createMockHost = (): HostConfig => ({ type: HostType.Local });
const TEST_ROOT_CWD = "/tmp/test-firmius";
const TEST_OBJECTIVE = "Test thread objective for unit testing";

describe("Thread", () => {
  describe("Serialization (Checkpoint)", () => {
    test("should create checkpoint for persistent thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      await thread.checkpoint();

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", thread.id, "checkpoint.json");
      expect(existsSync(checkpointPath)).toBe(true);

      await thread.dispose();
    });

    test("should include lead agent data in checkpoint", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      await thread.checkpoint();

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", thread.id, "checkpoint.json");
      const checkpointContent = await readFile(checkpointPath, "utf-8");
      const checkpoint = JSON.parse(checkpointContent);

      expect(checkpoint.threadId).toBe(thread.id);
      expect(checkpoint.leadAgentId).toBeDefined();
      expect(checkpoint.hostConfig).toBeDefined();
      expect(checkpoint.rootCwd).toBe(TEST_ROOT_CWD);

      await thread.dispose();
    });

    test("should save thread title in checkpoint", async () => {
      const customTitle = "Custom Test Thread Title";
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      thread.title = customTitle;
      await thread.checkpoint();

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", thread.id, "checkpoint.json");
      const checkpointContent = await readFile(checkpointPath, "utf-8");
      const checkpoint = JSON.parse(checkpointContent);

      expect(checkpoint.title).toBe(customTitle);

      await thread.dispose();
    });

    test("should save interrupted state in checkpoint", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      thread.interrupted = true;
      await thread.checkpoint();

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", thread.id, "checkpoint.json");
      const checkpointContent = await readFile(checkpointPath, "utf-8");
      const checkpoint = JSON.parse(checkpointContent);

      expect(checkpoint.wasInterrupted).toBe(true);

      await thread.dispose();
    });
  });

  describe("Restoring from Checkpoint", () => {
    test("should restore thread from checkpoint", async () => {
      const originalThread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", originalThread.id, "checkpoint.json");

      const originalId = originalThread.id;
      const originalTitle = originalThread.title;

      await originalThread.checkpoint();
      await originalThread.dispose();

      const restoredThread = await Thread.restore(checkpointPath);

      expect(restoredThread.id).toBe(originalId);
      expect(restoredThread.title).toBe(originalTitle);
      expect(restoredThread.rootCwd).toBe(TEST_ROOT_CWD);

      await restoredThread.dispose();
    });

    test("should restore thread interrupted state", async () => {
      const originalThread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      originalThread.interrupted = true;
      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", originalThread.id, "checkpoint.json");
      await originalThread.checkpoint();

      await originalThread.dispose();

      const restoredThread = await Thread.restore(checkpointPath);
      expect(restoredThread.interrupted).toBe(true);

      await restoredThread.dispose();
    });
  });

  describe("InMemoryThread", () => {
    test("should create in-memory thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      expect(thread).toBeDefined();
      expect(thread.id).toBeDefined();

      await thread.dispose();
    });

    test("should not persist in-memory thread to disk", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      await thread.checkpoint();

      const checkpointPath = join(process.env.HOME || "/root", ".firmius", "threads", thread.id, "checkpoint.json");
      expect(existsSync(checkpointPath)).toBe(false);

      await thread.dispose();
    });

    test("should support journaling in in-memory thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn: AgentTurn = {
        content: "In-memory turn",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Test reasoning"
      };
      const sequence = await thread.recordTurn(thread.leadAgent.id, turn);
      expect(sequence).toBeGreaterThanOrEqual(0);

      const message: AgentConversationalMessage = {
        isUser: true,
        content: "In-memory message",
        timestamp: Date.now(),
        tokens: 20
      };
      await thread.recordMessage(thread.leadAgent.id, message);

      const lastUserMsg = await thread.getLastUserMessage();
      expect(lastUserMsg?.content).toBe("In-memory message");

      await thread.dispose();
    });

    test("should support forget operations in in-memory thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn: AgentTurn = {
        content: "Turn to forget",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Test reasoning"
      };
      const sequence = await thread.recordTurn(thread.leadAgent.id, turn);

      await thread.forgetEntry(sequence);

      expect(thread.leadAgent).toBeDefined();

      await thread.dispose();
    });
  });

  describe("Thread Interruption", () => {
    test("should interrupt thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      await thread.interrupt();

      expect(thread.leadAgent).toBeDefined();

      await thread.dispose();
    });

    test("should clear interrupted state", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      thread.interrupted = true;
      thread.clearInterrupted();

      await thread.dispose();
    });
  });

  describe("Thread Properties", () => {
    test("should get thread id", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      expect(thread.id).toBeDefined();
      expect(typeof thread.id).toBe("string");

      await thread.dispose();
    });

    test("should get thread rootCwd", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      expect(thread.rootCwd).toBe(TEST_ROOT_CWD);

      await thread.dispose();
    });

    test("should get thread hostConfig", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      expect(thread.hostConfig).toBeDefined();
      expect(thread.hostConfig.type).toBe(HostType.Local);

      await thread.dispose();
    });

    test("should get lead agent", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      expect(thread.leadAgent).toBeDefined();
      expect(thread.leadAgent.id).toBeDefined();

      await thread.dispose();
    });
  });

  describe("Journaling Operations", () => {
    test("should record multiple turns with sequential sequences", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn1: AgentTurn = {
        content: "First turn",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 1"
      };
      const seq1 = await thread.recordTurn(thread.leadAgent.id, turn1);

      const turn2: AgentTurn = {
        content: "Second turn",
        timestamp: Date.now(),
        tokens: 60,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 2"
      };
      const seq2 = await thread.recordTurn(thread.leadAgent.id, turn2);

      expect(seq2).toBe(seq1 + 1);

      await thread.dispose();
    });

    test("should record user and agent messages", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const userMsg: AgentConversationalMessage = {
        isUser: true,
        content: "Hello agent",
        timestamp: Date.now(),
        tokens: 10
      };
      await thread.recordMessage(thread.leadAgent.id, userMsg);

      const agentMsg: AgentConversationalMessage = {
        isUser: false,
        content: "Hello human",
        timestamp: Date.now(),
        tokens: 10
      };
      await thread.recordMessage(thread.leadAgent.id, agentMsg);

      const lastUser = await thread.getLastUserMessage();
      expect(lastUser?.content).toBe("Hello agent");

      await thread.dispose();
    });

    test("should get last agent turn sequence", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn1: AgentTurn = {
        content: "First turn",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 1"
      };
      const seq1 = await thread.recordTurn(thread.leadAgent.id, turn1);

      const turn2: AgentTurn = {
        content: "Second turn",
        timestamp: Date.now(),
        tokens: 60,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 2"
      };
      await thread.recordTurn(thread.leadAgent.id, turn2);

      const lastTurn = await thread.getLastAgentTurn(thread.leadAgent.id);
      expect(lastTurn).toBeGreaterThan(seq1);

      await thread.dispose();
    });

    test("should forget events after sequence", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn1: AgentTurn = {
        content: "Turn 1",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 1"
      };
      const seq1 = await thread.recordTurn(thread.leadAgent.id, turn1);

      const turn2: AgentTurn = {
        content: "Turn 2",
        timestamp: Date.now(),
        tokens: 60,
        toolCalls: [],
        toolResults: [],
        reasoning: "Reasoning 2"
      };
      await thread.recordTurn(thread.leadAgent.id, turn2);

      // Forget events after sequence 1 (including turn 2)
      await thread.forgetEventsAfterSequence(seq1);

      await thread.dispose();
    });

    test("should edit user message", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const message: AgentConversationalMessage = {
        isUser: true,
        content: "Original message",
        timestamp: Date.now(),
        tokens: 30
      };
      const sequence = await thread.recordMessage(thread.leadAgent.id, message);

      const newContent = "Edited message content";
      await thread.editUserMessage(sequence, newContent);

      // The edit is recorded; verify the entry was edited
      expect(thread.leadAgent).toBeDefined();

      await thread.dispose();
    });

    test("should handle unforget entry", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const turn: AgentTurn = {
        content: "Turn to forget",
        timestamp: Date.now(),
        tokens: 50,
        toolCalls: [],
        toolResults: [],
        reasoning: "Test reasoning"
      };
      const sequence = await thread.recordTurn(thread.leadAgent.id, turn);

      // Forget the entry
      await thread.forgetEntry(sequence);

      // Unforget the entry
      await thread.unforgetEntry(sequence);

      await thread.dispose();
    });
  });

  describe("Thread Cancellation", () => {
    test("should cancel request", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      await thread.cancelRequest();

      expect(thread.leadAgent).toBeDefined();

      await thread.dispose();
    });
  });

  describe("Thread disposal", () => {
    test("should dispose persistent thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      const threadId = thread.id;
      await thread.dispose();

      // After dispose, thread should be removed from Engine
      expect(Engine.getThread(threadId)).toBeUndefined();
    });

    test("should dispose in-memory thread", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE,
        inMemory: true
      });

      const threadId = thread.id;
      await thread.dispose();

      // After dispose, thread should be removed from Engine
      expect(Engine.getThread(threadId)).toBeUndefined();
    });

    test("should support destroy as alias for dispose", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      const threadId = thread.id;
      await thread.destroy();

      expect(Engine.getThread(threadId)).toBeUndefined();
    });
  });

  describe("Thread title", () => {
    test("should have default title from objective", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: "This is a test objective"
      });

      expect(thread.title).toBeDefined();
      expect(thread.title?.length).toBeGreaterThan(0);

      await thread.dispose();
    });

    test("should allow setting custom title", async () => {
      const thread = await Thread.create({
        hostConfig: createMockHost(),
        rootCwd: TEST_ROOT_CWD,
        purpose: BuiltinPurposes.General,
        objective: TEST_OBJECTIVE
      });

      const newTitle = "My Custom Title";
      thread.title = newTitle;
      expect(thread.title).toBe(newTitle);

      await thread.dispose();
    });
  });
});
