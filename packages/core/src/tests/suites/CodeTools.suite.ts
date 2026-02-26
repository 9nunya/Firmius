import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import { CodeExecuteTool } from "../../tools/CodeTools";
import { createToolContext, registerGlobalCleanup, createTempDir, trackTempResource } from "../test_utils";

interface CodeExecuteInput {
  language: "python" | "node" | "bun";
  code: string;
  install?: string[];
}

const SUITE_NAME = "code";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export async function runCodeToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;
    
    console.log(`[${hostName}] Running CodeTools tests...`);

    registerGlobalCleanup();
    describe(`CodeTools (${hostName})`, () => {
    let host: IHost;
    let testDir: string;

    beforeAll(async () => {
      host = await getHost();
      await host.init();
      testDir = createTempDir();
      trackTempResource(testDir);
      await host.mkdir(testDir);
    });

    afterAll(async () => {
      await host.destroy();
    });

    test("Python basic execution", async () => {
      const ctx = createToolContext(host);
      const input: CodeExecuteInput = {
        language: "python",
        code: "print('Hello from Python')"
      };
      const result = await CodeExecuteTool.execute(input, ctx);
      expect(result.success).toBe(true);
      expect(result.output?.stdout).toContain("Hello from Python");
    });

    test("Node.js basic execution", async () => {
      const ctx = createToolContext(host);
      const input: CodeExecuteInput = {
        language: "node",
        code: "console.log('Hello from Node.js')"
      };
      const result = await CodeExecuteTool.execute(input, ctx);
      expect(result.success).toBe(true);
      expect(result.output?.stdout).toContain("Hello from Node.js");
    });

    test("Python state isolation", async () => {
      const ctx = createToolContext(host);
      await CodeExecuteTool.execute({ language: "python", code: "x = 123" }, ctx);
      const result2 = await CodeExecuteTool.execute({ language: "python", code: "print('x' in dir())" }, ctx);
      if (result2.success) {
        expect(result2.output?.stdout.trim()).toBe("False");
      }
    });

    test("Error handling - Python syntax error", async () => {
      const ctx = createToolContext(host);
      const result = await CodeExecuteTool.execute({
        language: "python",
        code: "if True print('missing colon')"
      }, ctx);
      expect(result.output?.exitCode !== 0).toBe(true);
      const output = (result.output?.stdout || "") + (result.output?.stderr || "");
      expect(output.length > 0).toBe(true);
    });

    test("Error handling - Runtime error", async () => {
      const ctx = createToolContext(host);
      const result = await CodeExecuteTool.execute({
        language: "python",
        code: "1/0"
      }, ctx);
      expect(result.output?.exitCode !== 0).toBe(true);
      const output = (result.output?.stdout || "") + (result.output?.stderr || "");
      expect(output.length > 0).toBe(true);
    });

    test("SummarizeInput and summary", async () => {
      const ctx = createToolContext(host);
      const input: CodeExecuteInput = {
        language: "python",
        code: "print('test')"
      };
      expect(CodeExecuteTool.summarizeInput(input)).toContain("python");
      const result = await CodeExecuteTool.execute(input, ctx);
      const summary = CodeExecuteTool.summary!(result, input);
      expect(summary.length > 0).toBe(true);
    });
  });
}
