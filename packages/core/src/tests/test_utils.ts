import { BuiltinPurposes, AgentWorkType, type AgentContext, ToolScope, type ToolContext, type IAgent } from "@firmius/shared";
import type { IHost } from "@firmius/shared";
import { Agent, Coordinator } from "@firmius/core";
import path from "node:path";
import { tmpdir, homedir } from "node:os";
import { mkdtempSync, rmSync, existsSync, writeFileSync, mkdirSync } from "node:fs";

const tempResources = new Set<string>();

export function getCleanupFunction(): () => void {
  return () => {
    for (const resource of tempResources) {
      try {
        if (existsSync(resource)) {
          rmSync(resource, { recursive: true, force: true });
        }
      } catch {
      }
    }
    tempResources.clear();
  };
}

export function registerGlobalCleanup() {
  const { afterAll } = require("bun:test");
  afterAll(getCleanupFunction());
}

export function trackTempResource(resourcePath: string): void {
  tempResources.add(resourcePath);
}

export function createTempDir(prefix: string = "firmius-test-"): string {
  const tempDir = mkdtempSync(path.join(tmpdir(), prefix));
  trackTempResource(tempDir);
  return tempDir;
}

export function createTempFile(prefix: string = "firmius-test-", extension: string = ".tmp"): string {
  const tempDir = createTempDir();
  const filePath = path.join(tempDir, `${prefix}${Date.now()}${extension}`);
  writeFileSync(filePath, "");
  return filePath;
}

export function createCacheDir(cacheName: string): string {
  const cacheBase = path.join(homedir(), ".firmius");
  const cacheDir = path.join(cacheBase, cacheName);

  if (!existsSync(cacheBase)) {
    mkdirSync(cacheBase, { recursive: true });
  }

  if (!existsSync(cacheDir)) {
    mkdirSync(cacheDir, { recursive: true });
    trackTempResource(cacheDir);
  }

  return cacheDir;
}

export async function withTempDir<T>(callback: (dir: string) => Promise<T>): Promise<T> {
  const tempDir = createTempDir();
  try {
    return await callback(tempDir);
  } finally {
    try {
      if (existsSync(tempDir)) {
        rmSync(tempDir, { recursive: true, force: true });
      }
      tempResources.delete(tempDir);
    } catch {
    }
  }
}

export function withTempDirSync<T>(callback: (dir: string) => T): T {
  const tempDir = createTempDir();
  try {
    return callback(tempDir);
  } finally {
    try {
      if (existsSync(tempDir)) {
        rmSync(tempDir, { recursive: true, force: true });
      }
      tempResources.delete(tempDir);
    } catch {
    }
  }
}

// Mock Agent Context Factory
export function createMockAgentContext(host: IHost): AgentContext {
  return {
    identity: {
      id: "mock-agent",
      threadId: "mock-thread",
      parentId: undefined,
      subagentIds: [],
      purpose: BuiltinPurposes.General,
      objective: "Test Objective"
    },
    historyData: {
      history: Agent.createHistory(AgentWorkType.Conversational, {
        isUser: true,
        content: "Test",
        timestamp: Date.now(),
        tokens: 0
      }),
      reasoningHistory: [],
      reasoningHistoryLimit: 4
    },
    environment: {
      host: host,
      cwd: host.defaultCwd,
        permissions: {
          scopes: [ToolScope.FilesystemRead, ToolScope.FilesystemWrite, ToolScope.Process, ToolScope.Semantic, ToolScope.Web],
          allowOutsideCwd: true
        },
        attachedFiles: []
      },
    state: {
      status: 'idle',
      metrics: {
        totalTokens: 0,
        lastTurnTokens: 0,
        lastPromptTokens: 0,
        startTime: Date.now()
      },
      todos: [],
      nextTodoId: 1,
      ownedProcesses: []
    },
    execution: {
      generationOptions: { providerId: "mock", modelId: "mock" },
      maxContextChars: 200000,
      tags: {},
      disableCompaction: false,
      anchors: new Set<string>()
    },
    io: {}
  };
}

export function createToolContext(host: IHost, agent?: AgentContext, coordinator?: Coordinator): ToolContext {
  return {
    host,
    agent: (agent || createMockAgentContext(host)) as unknown as IAgent,
    coordinator: coordinator!  // Will be provided when needed for coordinator tests
  };
}
