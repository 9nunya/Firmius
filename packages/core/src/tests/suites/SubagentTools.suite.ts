import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import { ToolScope } from "@firmius/shared";
import type { IHost } from "@firmius/shared";
import { AllSubagentTools, SubagentKillTool, SubagentWaitTool, SubagentPollTool, SubagentNudgeTool, SubagentStatusTool } from "@firmius/core";

import { mkdir, rm } from "node:fs/promises";
import { join } from "node:path";

const SUITE_NAME = "subagent";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

const TEST_THREAD_ID = "test-subagent-tools-thread";
const TEST_BASE_DIR = join(
  "/tmp",
  "firmius-test-subagent-tools",
  TEST_THREAD_ID,
);

export function runSubagentToolsTests(
  hostName: string,
  getHost: () => Promise<IHost>,
) {
  if (!shouldRunSuite()) return;

  describe(`SubagentTools (${hostName})`, () => {
    let host: IHost;

    beforeAll(async () => {
      host = await getHost();
      await host.init();
      await mkdir(TEST_BASE_DIR, { recursive: true });
    });

    afterAll(async () => {
      await rm("/tmp/firmius-test-subagent-tools", {
        recursive: true,
        force: true,
      }).catch(() => {});
    });

    describe("Tool Metadata", () => {
      test("SubagentWaitTool should have correct metadata", () => {
        expect(SubagentWaitTool.metadata.name).toBe("subagent_wait");
        expect(SubagentWaitTool.metadata.scope).toBe(ToolScope.Orchestration);
        expect(SubagentWaitTool.metadata.description).toContain("wait");
      });

      test("SubagentPollTool should have correct metadata", () => {
        expect(SubagentPollTool.metadata.name).toBe("subagent_poll");
        expect(SubagentPollTool.metadata.scope).toBe(ToolScope.Orchestration);
        expect(SubagentPollTool.metadata.description).toContain("poll");
      });

      test("SubagentNudgeTool should have correct metadata", () => {
        expect(SubagentNudgeTool.metadata.name).toBe("subagent_nudge");
        expect(SubagentNudgeTool.metadata.scope).toBe(ToolScope.Orchestration);
        expect(SubagentNudgeTool.metadata.description).toContain("nudge");
      });

      test("SubagentStatusTool should have correct metadata", () => {
        expect(SubagentStatusTool.metadata.name).toBe("subagent_status");
        expect(SubagentStatusTool.metadata.scope).toBe(ToolScope.Orchestration);
        expect(SubagentStatusTool.metadata.description).toContain("status");
      });

      test("SubagentKillTool should have correct metadata", () => {
        expect(SubagentKillTool.metadata.name).toBe("subagent_kill");
        expect(SubagentKillTool.metadata.scope).toBe(ToolScope.Orchestration);
        expect(SubagentKillTool.metadata.description).toContain("kill");
      });

      test("AllSubagentTools should export all tools", () => {
        expect(AllSubagentTools.length).toBe(5);
      });
    });

    describe("SubagentWaitTool", () => {
      test("should have correct input schema", () => {
        const input = { agent: "test-agent" };
        const result = SubagentWaitTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
        }
      });

      test("should require agent field", () => {
        const input = {};
        const result = SubagentWaitTool.input.safeParse(input);
        expect(result.success).toBe(false);
      });
    });

    describe("SubagentPollTool", () => {
      test("should accept agent parameter", () => {
        const input = { agent: "test-agent" };
        const result = SubagentPollTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
        }
      });

      test("should accept empty input for all children", () => {
        const input = {};
        const result = SubagentPollTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data).toEqual({});
        }
      });
    });

    describe("SubagentNudgeTool", () => {
      test("should accept agent and message parameters", () => {
        const input = {
          agent: "test-agent",
          message: "Test nudge message",
          forceRestart: false,
        } as any;
        const result = SubagentNudgeTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
          expect(result.data.message).toBe("Test nudge message");
          expect(result.data.forceRestart).toBe(false);
        }
      });

      test("should accept agent only", () => {
        const input = {
          agent: "test-agent",
        } as any;
        const result = SubagentNudgeTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
        }
      });

      test("should accept forceRestart: true", () => {
        const input = {
          agent: "test-agent",
          forceRestart: true,
        } as any;
        const result = SubagentNudgeTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
          expect(result.data.forceRestart).toBe(true);
        }
      });
    });

    describe("SubagentStatusTool", () => {
      test("should accept agent parameter", () => {
        const input = { agent: "test-agent" };
        const result = SubagentStatusTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
        }
      });

      test("should accept empty input for self status", () => {
        const input = {};
        const result = SubagentStatusTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data).toEqual({});
        }
      });
    });

    describe("SubagentKillTool", () => {
      test("should accept agent and reason parameters", () => {
        const input = { agent: "test-agent", reason: "Test kill" };
        const result = SubagentKillTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
          expect(result.data.reason).toBe("Test kill");
        }
      });

      test("should accept only agent parameter", () => {
        const input = { agent: "test-agent" };
        const result = SubagentKillTool.input.safeParse(input);
        expect(result.success).toBe(true);
        if (result.success) {
          expect(result.data.agent).toBe("test-agent");
        }
      });

      test("should require agent field", () => {
        const input = {};
        const result = SubagentKillTool.input.safeParse(input);
        expect(result.success).toBe(false);
      });
    });

    describe("Input Summarization", () => {
      test("SubagentWaitTool should summarize input", () => {
        const input = { agent: "test-agent" };
        expect(SubagentWaitTool.summarizeInput(input)).toContain("wait");
        expect(SubagentWaitTool.summarizeInput(input)).toContain("test-agent");
      });

      test("SubagentPollTool should summarize input with agent", () => {
        const input = { agent: "test-agent" };
        expect(SubagentPollTool.summarizeInput(input)).toContain("poll");
        expect(SubagentPollTool.summarizeInput(input)).toContain("test-agent");
      });

      test("SubagentPollTool should summarize empty input", () => {
        expect(SubagentPollTool.summarizeInput({})).toContain("poll");
        expect(SubagentPollTool.summarizeInput({})).toContain("children");
      });

      test("SubagentNudgeTool should summarize input", () => {
        const input = {
          agent: "test-agent",
          message: "Hello",
          forceRestart: false,
        };
        expect(SubagentNudgeTool.summarizeInput(input)).toContain("nudge");
        expect(SubagentNudgeTool.summarizeInput(input)).toContain("test-agent");
      });

      test("SubagentStatusTool should summarize input with agent", () => {
        const input = { agent: "test-agent" };
        expect(SubagentStatusTool.summarizeInput(input)).toContain("status");
        expect(SubagentStatusTool.summarizeInput(input)).toContain(
          "test-agent",
        );
      });

      test("SubagentStatusTool should summarize empty input", () => {
        expect(SubagentStatusTool.summarizeInput({})).toContain("self");
      });

      test("SubagentKillTool should summarize input", () => {
        const input = { agent: "test-agent", reason: "Failed" };
        expect(SubagentKillTool.summarizeInput(input)).toContain("kill");
        expect(SubagentKillTool.summarizeInput(input)).toContain("test-agent");
      });
    });

    describe("Output Summarization", () => {
      test("SubagentWaitTool should summarize agent_finished output", () => {
        const output: any = {
          success: true,
          summary: "test-agent finished (completed)",
          output: {
            agentId: "test-agent",
            status: "completed",
            elapsedMs: 5000,
          }
        };
        const input = { agent: "test-agent" };
        expect(SubagentWaitTool.summary!(output, input)).toContain("completed");
        expect(SubagentWaitTool.summary!(output, input)).toContain("test-agent");
      });

      test("SubagentWaitTool should summarize error output", () => {
        const output: any = {
          success: false,
          summary: "error: Agent failed",
          error: "Agent failed"
        };
        const input = { agent: "test-agent" };
        const summary = SubagentWaitTool.summary!(output, input);
        expect(summary).toContain("error");
      });

      test("SubagentPollTool should summarize output", () => {
        const output: any = {
          success: true,
          summary: "Polled 1 agent(s)",
          output: {
            agents: [
              {
                id: "agent-1",
                status: "running",
                timeSinceLastTurnMs: 5000,
                timeSinceLastToolCallMs: 2000,
              },
            ],
          }
        };
        const input = { agent: "agent-1" };
        const summary = SubagentPollTool.summary!(output, input);
        expect(summary).toContain("Polled");
      });

      test("SubagentNudgeTool should summarize message_injected output", () => {
        const output: any = {
          success: true,
          summary: "nudged agent test-agent",
          output: {
            action: "message_injected",
            agentId: "test-agent",
          }
        };
        const input = { agent: "test-agent", forceRestart: false };
        expect(SubagentNudgeTool.summary!(output, input)).toContain("nudge");
      });

      test("SubagentNudgeTool should summarize restarted output", () => {
        const output: any = {
          success: true,
          summary: "restarted agent test-agent",
          output: {
            action: "restarted",
            agentId: "test-agent",
          }
        };
        const input = { agent: "test-agent", forceRestart: true };
        expect(SubagentNudgeTool.summary!(output, input)).toContain("restarted");
      });

      test("SubagentStatusTool should summarize successful output", () => {
        const output: any = {
          success: true,
          summary: "Status of test-agent: working",
          output: {
            agent: {
              id: "test-agent",
              readableName: "Test Agent",
              purpose: "general",
              objective: "Test objective",
              status: "working",
              lastHeartbeat: Date.now(),
              lastProgressUpdate: Date.now(),
              timeSinceHeartbeatMs: 1000,
              timeSinceProgressMs: 2000,
              stuck: false,
              stuckReason: null,
            },
          }
        };
        const input = { agent: "test-agent" };
        const summary = SubagentStatusTool.summary!(output, input);
        expect(summary).toContain("working");
      });

      test("SubagentKillTool should summarize successful output", () => {
        const output: any = {
          success: true,
          summary: "Killed agent test-agent",
          output: {
            agentId: "test-agent",
          }
        };
        const input = { agent: "test-agent", reason: "Test" };
        expect(SubagentKillTool.summary!(output, input)).toContain("Killed");
      });

      test("SubagentKillTool should summarize failed output", () => {
        const output: any = {
          success: false,
          summary: "Kill failed",
          error: "Agent not found"
        };
        const input = { agent: "test-agent" };
        const summary = SubagentKillTool.summary!(output, input);
        expect(summary).toContain("failed");
      });
    });
  });
}
