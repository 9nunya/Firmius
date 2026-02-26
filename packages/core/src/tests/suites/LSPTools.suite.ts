import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import {
  LSPLookupTool,
  LSPInspectTool,
  LSPFindTool,
} from "../../tools/LSPTools";
import { createToolContext, registerGlobalCleanup, createTempDir } from "../test_utils";
import path from "node:path";

const SUITE_NAME = "lsp";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runLSPToolsTests(hostName: string, getHost: () => Promise<IHost>) {
  if (!shouldRunSuite()) return;

  registerGlobalCleanup();
  describe(`LSPTools (${hostName})`, () => {
    let host: IHost;
    let testDir: string;
    const testFile = "index.ts";
    const otherFile = "util.ts";

    beforeAll(async () => {
      host = await getHost();
      await host.init();

      testDir = createTempDir();

      if (await host.exists(testDir)) {
        await host.remove(testDir);
      }
      await host.mkdir(testDir);

      await host.writeFile(path.join(testDir, otherFile), `
export function add(a: number, b: number): number {
  return a + b;
}
`);

      await host.writeFile(path.join(testDir, testFile), `
import { add } from "./util";
function main() {
  const result = add(1, 2);
  console.log(result);
}
main();
`);

      await host.writeFile(path.join(testDir, "tsconfig.json"), JSON.stringify({
        compilerOptions: {
          target: "ESNext",
          module: "CommonJS",
          esModuleInterop: true,
          strict: true
        }
      }, null, 2));
    });

    afterAll(async () => {
      await host.destroy();
    });

    test("lsp_symbols", async () => {
      const ctx = createToolContext(host);
      ctx.agent.environment.cwd = testDir;
      const res = await LSPInspectTool.execute({ path: testFile, operation: "symbols" }, ctx);
      expect(res.success).toBe(true);
      expect(Array.isArray(res.output?.symbols)).toBe(true);
    }, 30000);

    test("lsp_hover", async () => {
      const ctx = createToolContext(host);
      ctx.agent.environment.cwd = testDir;
      const res = await LSPLookupTool.execute({ path: testFile, line: 3, character: 10, operation: "hover" }, ctx);
      expect(res.success).toBe(true);
    }, 30000);

    test("lsp_definition", async () => {
      const ctx = createToolContext(host);
      ctx.agent.environment.cwd = testDir;
      const res = await LSPLookupTool.execute({ path: testFile, line: 3, character: 10, operation: "definition" }, ctx);
      expect(res.success).toBe(true);
    }, 30000);

    test("summarizeInput and summary", async () => {
      const ctx = createToolContext(host);
      ctx.agent.environment.cwd = testDir;

      const defInput = { path: testFile, line: 3, character: 10, operation: "definition" as const };
      expect(LSPLookupTool.summarizeInput(defInput)).toBe(`lsp_lookup: definition on ${testFile}`);
      const defRes = await LSPLookupTool.execute(defInput, ctx);
      expect(defRes.success).toBe(true);
      expect(LSPLookupTool.summary!(defRes, defInput)).toContain("definitions");
    }, 30000);

    describe("lsp_find_symbol", () => {
      test("finds symbols by name", async () => {
        const ctx = createToolContext(host);
        ctx.agent.environment.cwd = testDir;
        const res = await LSPFindTool.execute({ name: "add" }, ctx);
        expect(res.success).toBe(true);
        expect(Array.isArray(res.output?.definitions)).toBe(true);
      }, 60000);
    });

    describe("lsp_exports", () => {
      test("gets exports from file", async () => {
        const ctx = createToolContext(host);
        ctx.agent.environment.cwd = testDir;
        const res = await LSPInspectTool.execute({ path: otherFile, operation: "exports" }, ctx);
        expect(res.success).toBe(true);
        expect(Array.isArray(res.output?.exports)).toBe(true);
      }, 30000);
    });

    describe("lsp_file_summary", () => {
      test("gets file summary", async () => {
        const ctx = createToolContext(host);
        ctx.agent.environment.cwd = testDir;
        const res = await LSPInspectTool.execute({ path: testFile, operation: "summary" }, ctx);
        expect(res.success).toBe(true);
        expect(res.output?.summary).toBeDefined();
      }, 30000);
    });
  });
}
