import { describe, test, expect, beforeAll, spyOn } from "bun:test";
import type { IHost } from "@firmius/shared";
import { ContextManageTool } from "../../tools/ContextTools";
import { createToolContext } from "../test_utils";
import { Engine } from "../../Engine";

const SUITE_NAME = "context";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runContextToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    describe(`ContextTools (${hostName})`, () => {
        let host: IHost;

        beforeAll(async () => {
            host = await getHost();
            await host.init();

            // Mock loadProviders to skip ZAI_API_KEY validation
            spyOn(Engine, "loadProviders").mockImplementation(() => {});

            await Engine.ignite();
        });

        test("inspect_context", async () => {
            const ctx = createToolContext(host);
            const input = { operation: "inspect" as const };
            const res = await ContextManageTool.execute(input, ctx);
            
            expect(res.success).toBe(true);
            if (res.output && "metrics" in res.output) {
                expect(res.output.id).toBe(ctx.agent.identity.id);
                expect(res.output.metrics).toBeDefined();
                expect(res.output.lspAvailability).toBeDefined();
            }
        });

        test("summarizeInput and summary", async () => {
            const ctx = createToolContext(host);

            // ContextManageTool with operation: "inspect"
            const inspectInput = { operation: "inspect" as const };
            expect(ContextManageTool.summarizeInput(inspectInput)).toBe("context_manage: inspect");
            const inspectRes = await ContextManageTool.execute(inspectInput, ctx);
            expect(ContextManageTool.summary!(inspectRes, inspectInput)).toContain(ctx.agent.identity.id);

            // ContextManageTool with operation: "mark_anchor"
            const markInput = { operation: "mark_anchor" as const, decision: "Test decision" };
            expect(ContextManageTool.summarizeInput(markInput)).toBe(`context_manage: mark_anchor`);
            const markRes = await ContextManageTool.execute(markInput, ctx);
            expect(ContextManageTool.summary!(markRes, markInput)).toContain("Test decision");

            // ContextManageTool with operation: "set_limit"
            const limitInput = { operation: "set_limit" as const, limit: 5 };
            expect(ContextManageTool.summarizeInput(limitInput)).toBe("context_manage: set_limit");
            const limitRes = await ContextManageTool.execute(limitInput, ctx);
            expect(ContextManageTool.summary!(limitRes, limitInput)).toContain("5");
        });
    });
}
