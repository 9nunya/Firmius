import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import { FileReadTool, FileEditTool, FileQueryTool } from "../../tools/FileTools";
import { createToolContext, registerGlobalCleanup, createTempDir } from "../test_utils";
import path from "node:path";

const SUITE_NAME = "file";

function shouldRunSuite(): boolean {
    const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
    if (!envSuites || envSuites === "all") return true;
    return envSuites.split(",").includes(SUITE_NAME);
}

export function runFileToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    registerGlobalCleanup();
    describe(`FileTools (${hostName})`, () => {
        let host: IHost;
        let testDir: string;

        beforeAll(async () => {
            host = await getHost();
            await host.init();
            const tempDir = createTempDir();
            testDir = path.resolve(host.defaultCwd.toString(), tempDir.split(path.sep).pop()!);
            await host.mkdir(testDir);
        });

        afterAll(async () => {
            await host.destroy();
        });

        test("write_file & exists", async () => {
            const ctx = createToolContext(host);
            const filePath = path.resolve(testDir, "test.txt");

            const writeRes = await FileEditTool.execute({ operation: "write", file: filePath, content: "Hello World" }, ctx);
            expect(writeRes.success).toBe(true);

            const existsRes = await FileQueryTool.execute({ operation: "exists", path: filePath }, ctx);
            expect(existsRes.output?.exists).toBe(true);

            const content = await host.readFile(filePath);
            expect(content).toBe("Hello World");
        });

        test("list_dir", async () => {
            const ctx = createToolContext(host);
            const subDir = path.resolve(testDir, "subdir");
            await host.mkdir(subDir);

            const listRes = await FileQueryTool.execute({ operation: "list", path: testDir }, ctx);
            expect(listRes.success).toBe(true);

            const fileEntry = listRes.output?.entries?.find(e => e.name === "test.txt");
            expect(fileEntry).toBeDefined();
            expect(fileEntry!.isDirectory).toBe(false);
            expect(fileEntry!.size).toBe(11); // "Hello World"
            expect(fileEntry!.mtime).toBeGreaterThan(0);
            expect(fileEntry!.mode).toBeDefined();

            const dirEntry = listRes.output?.entries?.find(e => e.name === "subdir");
            expect(dirEntry).toBeDefined();
            expect(dirEntry!.isDirectory).toBe(true);
        });

        test("search_dir", async () => {
            const ctx = createToolContext(host);
            const searchRes = await FileQueryTool.execute({ operation: "search", path: testDir, query: "Hello" }, ctx);

            if (!searchRes.success) console.log(`[DEBUG] search_dir failed: ${searchRes.error}`);
            expect(searchRes.success).toBe(true);
            expect(searchRes.output?.matches?.length ?? 0).toBeGreaterThan(0);
            expect(searchRes.output?.matches?.[0]?.content).toContain("Hello World");
        });

        test("file_read", async () => {
            const ctx = createToolContext(host);
            const filePath = path.resolve(testDir, "test.txt");

            const readRes = await FileReadTool.execute({ file: filePath }, ctx);
            expect(readRes.success).toBe(true);
            expect(readRes.output?.content).toBe("Hello World");
        });

        test("summarizeInput and summary", async () => {
            const ctx = createToolContext(host);
            const filePath = path.resolve(testDir, "test.txt");

            // FileEditTool (write)
            const writeInput = { operation: "write" as const, file: filePath, content: "Hello World" };
            expect(FileEditTool.summarizeInput(writeInput)).toBe(`"${filePath}" [write]`);
            const writeRes = await FileEditTool.execute(writeInput, ctx);
            expect(FileEditTool.summary!(writeRes, writeInput)).toContain(filePath);

            // FileReadTool
            const readInput = { file: filePath, startLine: 1, endLine: 5 };
            expect(FileReadTool.summarizeInput(readInput)).toBe(`"${filePath}" [L1-5]`);
            const readRes = await FileReadTool.execute(readInput, ctx);
            expect(FileReadTool.summary!(readRes, readInput)).toContain(filePath);

            // FileQueryTool (list)
            const listInput = { operation: "list" as const, path: testDir };
            expect(FileQueryTool.summarizeInput(listInput)).toBe(`"${testDir}" [list]`);
            const listRes = await FileQueryTool.execute(listInput, ctx);
            expect(FileQueryTool.summary!(listRes, listInput)).toContain(testDir);

            // FileQueryTool (exists)
            const existsInput = { operation: "exists" as const, path: filePath };
            expect(FileQueryTool.summarizeInput(existsInput)).toBe(`"${filePath}" [exists]`);
            const existsRes = await FileQueryTool.execute(existsInput, ctx);
            expect(FileQueryTool.summary!(existsRes, existsInput)).toContain(filePath);

            // FileEditTool (replace)
            const replaceInput = { operation: "replace" as const, file: filePath, search: "World", replace: "Universe" };
            expect(FileEditTool.summarizeInput(replaceInput)).toBe(`"${filePath}" [replace]`);
            const replaceRes = await FileEditTool.execute(replaceInput, ctx);
            expect(FileEditTool.summary!(replaceRes, replaceInput)).toContain(filePath);

            // FileQueryTool (search)
            const searchInput = { operation: "search" as const, path: testDir, query: "Universe" };
            expect(FileQueryTool.summarizeInput(searchInput)).toBe(`"Universe" in "${testDir}" [search]`);
            const searchRes = await FileQueryTool.execute(searchInput, ctx);
            expect(FileQueryTool.summary!(searchRes, searchInput)).toContain(testDir);
        });
    });
}
