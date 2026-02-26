import { describe, test, expect, beforeAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import { AgentDelegateTool } from "../../tools/DelegationTools";
import { Engine } from "../../Engine";
import { BuiltinPurposes } from "@firmius/shared";

const SUITE_NAME = "delegation";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runDelegationToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    describe(`DelegationTools (${hostName})`, () => {
        let host: IHost;

        beforeAll(async () => {
            host = await getHost();
            await host.init();

            // Clear the agents map and don't ignite engine to avoid recursion
            Engine.agentFactory.agents.clear();
        });

        test("summarizeInput", async () => {
            const delegateInput = {
                title: "Test Sub-agent",
                objective: "Test sub-agent",
                purpose: BuiltinPurposes.Coder,
                agentId: "test-summary",
                blocking: true,
                host: "inherit" as const,
                allowOutsideCwd: true
            };
            expect(AgentDelegateTool.summarizeInput(delegateInput)).toBe(`delegate [test-summary] (blocking): Test Sub-agent`);
        });

        test("summary on failed", async () => {
            const mockOutput: any = {
                success: false,
                summary: "failed - Some error",
                error: "Some error"
            };
            const delegateInput = {
                title: "Test Sub-agent",
                objective: "Test sub-agent",
                purpose: BuiltinPurposes.Coder,
                agentId: "test-failed",
                blocking: true,
                host: "inherit" as const,
                allowOutsideCwd: true
            };
            expect(AgentDelegateTool.summary!(mockOutput, delegateInput)).toContain("failed");
        });

        test("summary on completed", async () => {
            const mockOutput: any = {
                success: true,
                summary: "Agent test-completed completed.",
                output: {
                    agentId: "test-completed",
                    status: "completed",
                    result: "Some result"
                }
            };
            const delegateInput = {
                title: "Test Sub-agent",
                objective: "Test sub-agent",
                purpose: BuiltinPurposes.Coder,
                agentId: "test-completed",
                blocking: true,
                host: "inherit" as const,
                allowOutsideCwd: true
            };
            expect(AgentDelegateTool.summary!(mockOutput, delegateInput)).toContain("completed");
        });
    });
}

// Backward compatibility alias
/** @deprecated Use runDelegationToolsTests instead */
export const runRecursionToolsTests = runDelegationToolsTests;
