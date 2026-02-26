import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import { ProcessControlTool, ProcessExecuteTool, ProcessWaitTool } from "../../tools/ProcessTools";
import { createToolContext } from "../test_utils";
import { Engine } from "../../Engine";
import type { ProcessControlInput, ProcessWaitInput, ProcessExecuteInput } from "../../tools/ProcessTools";

const SUITE_NAME = "process";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runProcessToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    describe(`ProcessTools (${hostName})`, () => {
        let host: IHost;

        beforeAll(async () => {
            host = await getHost();
            await host.init();
        });

        afterAll(async () => {
            await host.destroy();
        });

        test("spawn & list & kill", async () => {
            const ctx = createToolContext(host);

            // Spawn a long running process
            const spawnInput: ProcessControlInput = { operation: "spawn", command: "sleep", args: ["10"] };
            const spawnRes = await ProcessControlTool.execute(spawnInput, ctx);
            expect(spawnRes.success).toBe(true);
            const pid = (spawnRes.output as any).pid!;
            expect(pid).toBeDefined();
            expect(ctx.agent.state.ownedProcesses).toContain(pid);

            // List
            const listInput: ProcessControlInput = { operation: "list" };
            const listRes = await ProcessControlTool.execute(listInput, ctx);
            expect((listRes.output as any).processes.some((p: any) => p.pid === pid)).toBe(true);

            // Kill
            const killInput: ProcessControlInput = { operation: "kill", pid };
            const killRes = await ProcessControlTool.execute(killInput, ctx);
            expect(killRes.success).toBe(true);

            // Verify removed from owned
            expect(ctx.agent.state.ownedProcesses).not.toContain(pid);
            expect(Engine.processManager.get(pid)).toBeUndefined();
        });

        test("spawn echo (stdout capture)", async () => {
            const ctx = createToolContext(host);
            const spawnInput: ProcessControlInput = { operation: "spawn", command: "echo", args: ["test_echo"] };
            const spawnRes = await ProcessControlTool.execute(spawnInput, ctx);
            expect(spawnRes.success).toBe(true);
            const pid = (spawnRes.output as any).pid!;

            const waitInput: ProcessWaitInput = { operation: "exit", pid, timeoutMs: 2000 };
            const waitRes = await ProcessWaitTool.execute(waitInput, ctx);
            expect(waitRes.success).toBe(true);
            expect((waitRes.output as any).stdout).toContain("test_echo");
        });

        test("send_keys & wait_for_output", async () => {
            const ctx = createToolContext(host);

            // Spawn sh (interactive shell)
            const spawnInput: ProcessControlInput = { operation: "spawn", command: "sh" };
            const spawnRes = await ProcessControlTool.execute(spawnInput, ctx);
            const pid = (spawnRes.output as any).pid!;

            // Send input: echo and wait for output
            const sendKeysInput: ProcessControlInput = { operation: "send_keys", pid, keys: "echo 'hello_from_keys'\n" };
            await ProcessControlTool.execute(sendKeysInput, ctx);

            // Wait for output
            const waitOutputInput: ProcessWaitInput = {
                operation: "output",
                pid,
                regex: "hello_from_keys",
                timeoutMs: 5000
            };
            const waitRes = await ProcessWaitTool.execute(waitOutputInput, ctx);

            if (!waitRes.success) {
                console.log(`[DEBUG] WaitForOutput failed in ${hostName}: ${waitRes.error}`);
                console.log(`[DEBUG] Current stdout: ${Engine.processManager.get(pid)?.stdout}`);
            }

            expect(waitRes.success).toBe(true);
            expect((waitRes.output as any).match).toContain("hello_from_keys");

            const killInput: ProcessControlInput = { operation: "kill", pid };
            await ProcessControlTool.execute(killInput, ctx);
        });

        test("wait timeout & exit code", async () => {
            const ctx = createToolContext(host);

            // 1. Timeout case
            // Sleep 5s, timeout 200ms. Should definitely timeout.
            const spawnInput1: ProcessControlInput = { operation: "spawn", command: "sleep", args: ["5"] };
            const spawnRes1 = await ProcessControlTool.execute(spawnInput1, ctx);
            const pid1 = (spawnRes1.output as any).pid!;

            const waitInput1: ProcessWaitInput = { operation: "exit", pid: pid1, timeoutMs: 200 };
            const waitRes1 = await ProcessWaitTool.execute(waitInput1, ctx);

            expect(waitRes1.success).toBe(false);
            expect(waitRes1.error).toContain("timeout");

            const killInput1: ProcessControlInput = { operation: "kill", pid: pid1 };
            await ProcessControlTool.execute(killInput1, ctx);

            // 2. Natural exit case
            const spawnInput2: ProcessControlInput = { operation: "spawn", command: "echo", args: ["done"] };
            const spawnRes2 = await ProcessControlTool.execute(spawnInput2, ctx);
            const pid2 = (spawnRes2.output as any).pid!;

            // Wait for it
            const waitInput2: ProcessWaitInput = { operation: "exit", pid: pid2, timeoutMs: 2000 };
            const waitRes2 = await ProcessWaitTool.execute(waitInput2, ctx);
            expect(waitRes2.success).toBe(true);
            expect((waitRes2.output as any).exitCode).toBe(0);
            expect((waitRes2.output as any).stdout).toContain("done");
        });

        test("resize (smoke test)", async () => {
            const ctx = createToolContext(host);
            const spawnInput: ProcessControlInput = { operation: "spawn", command: "sh" };
            const spawnRes = await ProcessControlTool.execute(spawnInput, ctx);
            const pid = (spawnRes.output as any).pid!;

            const resizeInput: ProcessControlInput = { operation: "resize", pid, cols: 100, rows: 40 };
            const resizeRes = await ProcessControlTool.execute(resizeInput, ctx);
            expect(resizeRes.success).toBe(true);

            const killInput: ProcessControlInput = { operation: "kill", pid };
            await ProcessControlTool.execute(killInput, ctx);
        });

        test("summarizeInput and summary", async () => {
            const ctx = createToolContext(host);

            // ProcessControlTool (spawn)
            const spawnInput: ProcessControlInput = { operation: "spawn", command: "echo", args: ["test"] };
            expect(ProcessControlTool.summarizeInput(spawnInput)).toBe(`process_control: spawn`);
            const spawnRes = await ProcessControlTool.execute(spawnInput, ctx);
            expect((spawnRes.output as any).pid).toBeDefined();
            expect(ProcessControlTool.summary!(spawnRes, spawnInput)).toContain((spawnRes.output as any).pid!);

            // ProcessControlTool (kill)
            const killInput: ProcessControlInput = { operation: "kill", pid: (spawnRes.output as any).pid! };
            expect(ProcessControlTool.summarizeInput(killInput)).toBe(`process_control: kill`);
            const killRes = await ProcessControlTool.execute(killInput, ctx);
            expect(ProcessControlTool.summary!(killRes, killInput)).toContain((spawnRes.output as any).pid!);

            // ProcessControlTool (send_keys)
            const spawn2Input: ProcessControlInput = { operation: "spawn", command: "sh" };
            const spawn2Res = await ProcessControlTool.execute(spawn2Input, ctx);
            const pid2 = (spawn2Res.output as any).pid!;
            const sendKeysInput: ProcessControlInput = { operation: "send_keys", pid: pid2, keys: "echo test\n" };
            expect(ProcessControlTool.summarizeInput(sendKeysInput)).toBe(`process_control: send_keys`);
            const sendKeysRes = await ProcessControlTool.execute(sendKeysInput, ctx);
            expect(ProcessControlTool.summary!(sendKeysRes, sendKeysInput)).toContain(pid2);
            await ProcessControlTool.execute({ operation: "kill", pid: pid2 }, ctx);

            // ProcessWaitTool (exit)
            const spawn3Input: ProcessControlInput = { operation: "spawn", command: "echo", args: ["done"] };
            const spawn3Res = await ProcessControlTool.execute(spawn3Input, ctx);
            const pid3 = (spawn3Res.output as any).pid!;
            const waitExitInput: ProcessWaitInput = { operation: "exit", pid: pid3, timeoutMs: 5000 };
            expect(ProcessWaitTool.summarizeInput(waitExitInput)).toBe(`process_wait: exit`);
            const waitExitRes = await ProcessWaitTool.execute(waitExitInput, ctx);
            expect(ProcessWaitTool.summary!(waitExitRes, waitExitInput)).toContain(pid3);

            // ProcessWaitTool (output)
            const spawn4Input: ProcessControlInput = { operation: "spawn", command: "sh" };
            const spawn4Res = await ProcessControlTool.execute(spawn4Input, ctx);
            const pid4 = (spawn4Res.output as any).pid!;
            await ProcessControlTool.execute({ operation: "send_keys", pid: pid4, keys: "hello\n" }, ctx);
            const waitOutputInput: ProcessWaitInput = { operation: "output", pid: pid4, regex: "hello", timeoutMs: 5000 };
            expect(ProcessWaitTool.summarizeInput(waitOutputInput)).toBe(`process_wait: output`);
            const waitOutputRes = await ProcessWaitTool.execute(waitOutputInput, ctx);
            expect(ProcessWaitTool.summary!(waitOutputRes, waitOutputInput)).toContain(pid4);
            await ProcessControlTool.execute({ operation: "kill", pid: pid4 }, ctx);

            // ProcessControlTool (list)
            const listInput: ProcessControlInput = { operation: "list" };
            expect(ProcessControlTool.summarizeInput(listInput)).toBe(`process_control: list`);
            const listRes = await ProcessControlTool.execute(listInput, ctx);
            expect(ProcessControlTool.summary!(listRes, listInput)).toContain("processes");

            // ProcessControlTool (resize)
            const spawn5Input: ProcessControlInput = { operation: "spawn", command: "sh" };
            const spawn5Res = await ProcessControlTool.execute(spawn5Input, ctx);
            const pid5 = (spawn5Res.output as any).pid!;
            const resizeInput: ProcessControlInput = { operation: "resize", pid: pid5, cols: 80, rows: 24 };
            expect(ProcessControlTool.summarizeInput(resizeInput)).toBe(`process_control: resize`);
            const resizeRes = await ProcessControlTool.execute(resizeInput, ctx);
            expect(ProcessControlTool.summary!(resizeRes, resizeInput)).toContain(pid5);
            await ProcessControlTool.execute({ operation: "kill", pid: pid5 }, ctx);

            // ProcessExecuteTool
            const runInput: ProcessExecuteInput = { command: "echo", args: ["run"] };
            expect(ProcessExecuteTool.summarizeInput(runInput)).toBe(`"echo run"`);
            const runRes = await ProcessExecuteTool.execute(runInput, ctx);
            expect(ProcessExecuteTool.summary!(runRes, runInput)).toContain(runInput.command);
        });
    });
}
