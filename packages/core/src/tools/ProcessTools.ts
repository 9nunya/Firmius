import { z } from "zod";
import type { ITool, ToolContext, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";
import { Engine } from "@firmius/core";
import { getTerminalSequence } from "@firmius/shared/utils";
import { analyzeCommand } from "@firmius/core/security";

function checkBlockedCommand(fullCmd: string): { blocked: boolean; redirect?: string } {
  const analysis = analyzeCommand(fullCmd);
  return {
    blocked: !analysis.allowed,
    redirect: analysis.redirect
  };
}

// ============================================================================
// Process Control Tool (spawn, kill, list, resize, send_keys)
// ============================================================================

const ProcessControlSpawnInputSchema = z.object({
  operation: z.literal("spawn"),
  command: z.string().describe("Command to run"),
  args: z.array(z.string()).optional().describe("Arguments"),
  cwd: z.string().optional().describe("Working directory")
});

const ProcessControlKillInputSchema = z.object({
  operation: z.literal("kill"),
  pid: z.string().describe("Process PID to kill")
});

const ProcessControlListInputSchema = z.object({
  operation: z.literal("list")
});

const ProcessControlResizeInputSchema = z.object({
  operation: z.literal("resize"),
  pid: z.string().describe("Process PID"),
  cols: z.number().describe("Number of columns"),
  rows: z.number().describe("Number of rows")
});

const ProcessControlSendKeysInputSchema = z.object({
  operation: z.literal("send_keys"),
  pid: z.string().describe("Process PID"),
  keys: z.string().describe("Input string or key combo (e.g. 'ls', '<ctrl+c>', '<enter>')")
});

const ProcessControlInputSchema = z.discriminatedUnion("operation", [
  ProcessControlSpawnInputSchema,
  ProcessControlKillInputSchema,
  ProcessControlListInputSchema,
  ProcessControlResizeInputSchema,
  ProcessControlSendKeysInputSchema
]);

export type ProcessControlInput = z.infer<typeof ProcessControlInputSchema>;

export interface ProcessControlOutput {
  pid?: string;
  processes?: { pid: string; completed: boolean }[];
  message?: string;
}

async function handleSpawn(
  input: z.infer<typeof ProcessControlSpawnInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessControlOutput>> {
  try {
    const fullCmd = input.args ? `${input.command} ${input.args.join(" ")}` : input.command;

    const { blocked, redirect } = checkBlockedCommand(fullCmd);
    if (blocked) {
      return { success: false, summary: "Command blocked", error: `COMMAND BLOCKED: ${redirect}` };
    }

    const cwd = input.cwd || context.host.defaultCwd.toString();
    const handle = await context.host.spawn(fullCmd, { cwd: cwd });

    const pid = Engine.processManager.register(handle);
    Engine.processManager.setCommand(pid, fullCmd);
    context.agent.state.ownedProcesses.push(pid);

    // Update tool call metadata immediately
    if (context.threadId && context.toolCallId) {
       Engine.emitToolCallUpdate(context.threadId, {
         agentId: context.agent.id,
         callId: context.toolCallId,
         metadata: { processPid: handle.pid, processId: pid },
         summary: `Spawned: ${fullCmd}`
       });
    }

    // Hook streaming output for persistent background processes
    handle.onOutput((data, src) => {
      if (context.threadId) {
        Engine.emitProcessOutput(context.threadId, {
          processId: pid,
          pid: handle.pid,
          data,
          source: src
        });
      }
    });

    // Hook process exit
    handle.wait().then(res => {
      if (context.threadId) {
        Engine.emitProcessExit(context.threadId, {
          processId: pid,
          pid: handle.pid,
          exitCode: res.exitCode,
          durationMs: res.elapsed
        });
      }
    }).catch(() => { });

    return { success: true, summary: `Process spawned with PID ${pid}`, output: { pid, message: "Process spawned successfully." } };
  } catch (e: any) {
    return { success: false, summary: "Spawn failed", error: e.message };
  }
}

async function handleKill(
  input: z.infer<typeof ProcessControlKillInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessControlOutput>> {
  if (!context.agent.state.ownedProcesses.includes(input.pid)) {
    return { success: false, summary: "Access denied", error: `Access denied or process ${input.pid} not found` };
  }

  const handle = Engine.processManager.get(input.pid);
  if (!handle) return { success: false, summary: "Not found", error: `Process ${input.pid} not found in manager` };

  await handle.kill("SIGKILL");
  Engine.processManager.unregister(input.pid);
  context.agent.state.ownedProcesses = context.agent.state.ownedProcesses.filter((p) => p !== input.pid);

  return { success: true, summary: `Process ${input.pid} killed`, output: { message: `Process ${input.pid} killed.` } };
}

async function handleList(
  context: ToolContext
): Promise<ToolResult<ProcessControlOutput>> {
  const all = Engine.processManager.list();
  const owned = all.filter((p: any) => context.agent.state.ownedProcesses.includes(p.id));
  const processes = owned.map((p: any) => ({ pid: p.id, completed: p.completed }));
  return { success: true, summary: `Managing ${processes.length} processes`, output: { processes } };
}

async function handleResize(
  input: z.infer<typeof ProcessControlResizeInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessControlOutput>> {
  if (!context.agent.state.ownedProcesses.includes(input.pid)) {
    return { success: false, summary: "Access denied", error: `Access denied or process ${input.pid} not found` };
  }
  const handle = Engine.processManager.get(input.pid);
  if (!handle) return { success: false, summary: "Not found", error: `Process ${input.pid} not found` };
  handle.resize(input.cols, input.rows);
  return { success: true, summary: `Resized ${input.pid}`, output: { message: `Resized ${input.pid} to ${input.cols}x${input.rows}` } };
}

async function handleSendKeys(
  input: z.infer<typeof ProcessControlSendKeysInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessControlOutput>> {
  if (!context.agent.state.ownedProcesses.includes(input.pid)) {
    return { success: false, summary: "Access denied", error: `Access denied or process ${input.pid} not found` };
  }
  const handle = Engine.processManager.get(input.pid);
  if (!handle) return { success: false, summary: "Not found", error: `Process ${input.pid} not found` };

  let text = input.keys;
  if (input.keys.startsWith("<") && input.keys.endsWith(">")) {
    text = getTerminalSequence(input.keys.slice(1, -1));
  }

  handle.write(text);
  return { success: true, summary: `Sent input to ${input.pid}`, output: { message: `Sent input to ${input.pid}` } };
}

export const ProcessControlTool: ITool<ProcessControlInput, ProcessControlOutput> = {
  metadata: {
    name: "process_control",
    description: "Control background processes.",
    scope: ToolScope.Process
  },
  input: ProcessControlInputSchema,
  execute: async (input: ProcessControlInput, context: ToolContext): Promise<ToolResult<ProcessControlOutput>> => {
    switch (input.operation) {
      case "spawn":
        return handleSpawn(input, context);
      case "kill":
        return handleKill(input, context);
      case "list":
        return handleList(context);
      case "resize":
        return handleResize(input, context);
      case "send_keys":
        return handleSendKeys(input, context);
      default:
        return { success: false, summary: "Invalid operation", error: `Unknown operation: ${(input as any).operation}` };
    }
  },
  summarizeInput: (input: ProcessControlInput) => {
    return `process_control: ${input.operation}`;
  },
  summary: (output: ToolResult<ProcessControlOutput>) => {
    return output.summary;
  }
};

// ============================================================================
// Process Execute Tool (run_command)
// ============================================================================

export interface ProcessExecuteInput {
  command: string;
  args?: string[];
  cwd?: string;
  timeoutMs?: number;
}

export interface ProcessExecuteOutput {
  exitCode: number;
  stdout: string;
  stderr: string;
}

const ProcessExecuteInputSchema = z.object({
  command: z.string().describe("The command to run."),
  args: z.array(z.string()).optional().describe("Command arguments."),
  cwd: z.string().optional().describe("Working directory for the command."),
  timeoutMs: z.number().default(60000).describe("Timeout in milliseconds.")
});

export const ProcessExecuteTool: ITool<ProcessExecuteInput, ProcessExecuteOutput> = {
  metadata: {
    name: "process_execute",
    description: "Run a command and wait for it to complete.",
    scope: ToolScope.Process
  },
  input: ProcessExecuteInputSchema,
  execute: async (input: ProcessExecuteInput, context: ToolContext): Promise<ToolResult<ProcessExecuteOutput>> => {
    try {
      const fullCmd = input.args ? `${input.command} ${input.args.join(" ")}` : input.command;

      const { blocked, redirect } = checkBlockedCommand(fullCmd);
      if (blocked) {
        return {
          success: false,
          summary: "Command blocked",
          error: `COMMAND BLOCKED: ${redirect}`,
          output: { exitCode: -1, stdout: "", stderr: "Blocked" }
        };
      }

      const cwd = input.cwd || context.host.defaultCwd.toString();
      const handle = await context.host.spawn(fullCmd, { cwd });

      let stdout = "";
      let stderr = "";

      handle.onOutput((data, src) => {
        if (src === 'stdout') stdout += data;
        else stderr += data;

        if (context.threadId) {
          Engine.emitProcessOutput(context.threadId, {
            processId: context.toolCallId || String(handle.pid),
            pid: handle.pid,
            data,
            source: src
          });
        }
      });

      const res = await handle.wait();

      if (context.threadId) {
        Engine.emitProcessExit(context.threadId, {
          processId: context.toolCallId || String(handle.pid),
          pid: handle.pid,
          exitCode: res.exitCode,
          durationMs: res.elapsed
        });
      }

      return {
        success: res.exitCode === 0,
        summary: `Command exited with ${res.exitCode}`,
        output: {
          exitCode: res.exitCode,
          stdout: stdout || res.stdout,
          stderr: stderr || res.stderr
        },
        error: res.exitCode === 0 ? undefined : (stderr || res.stderr)
      };
    } catch (e: any) {
      return {
        success: false,
        summary: "Execution failed",
        error: e.message,
        output: { exitCode: -1, stdout: "", stderr: e.message }
      };
    }
  },
  summarizeInput: (input: ProcessExecuteInput) => {
    return `"${input.command}"`;
  },
  summary: (output: ToolResult<ProcessExecuteOutput>) => {
    return output.summary;
  }
};

// ============================================================================
// Process Wait Tool (wait_process + wait_for_output)
// ============================================================================

const ProcessWaitExitInputSchema = z.object({
  operation: z.literal("exit"),
  pid: z.string().describe("Process PID"),
  timeoutMs: z.number().default(5000).describe("Timeout in milliseconds")
});

const ProcessWaitOutputInputSchema = z.object({
  operation: z.literal("output"),
  pid: z.string().describe("Process PID"),
  regex: z.string().describe("Regex pattern to wait for"),
  timeoutMs: z.number().default(5000).describe("Timeout in milliseconds")
});

const ProcessWaitInputSchema = z.discriminatedUnion("operation", [
  ProcessWaitExitInputSchema,
  ProcessWaitOutputInputSchema
]);

export type ProcessWaitInput = z.infer<typeof ProcessWaitInputSchema>;

export interface ProcessWaitOutput {
  exitCode?: number;
  stdout?: string;
  stderr?: string;
  match?: string;
  message?: string;
}

async function handleWaitExit(
  input: z.infer<typeof ProcessWaitExitInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessWaitOutput>> {
  if (!context.agent.state.ownedProcesses.includes(input.pid)) {
    return { success: false, summary: "Access denied", error: `Access denied or process ${input.pid} not found` };
  }
  const handle = Engine.processManager.get(input.pid);
  if (!handle) return { success: false, summary: "Not found", error: `Process ${input.pid} not found` };

  const timeoutPromise = new Promise<{ timeout: true }>((resolve) =>
    setTimeout(() => resolve({ timeout: true }), input.timeoutMs)
  );

  try {
    const res = await Promise.race([handle.wait(), timeoutPromise]);

    if ("timeout" in res) {
      return { success: false, summary: "Wait timeout", error: "Timeout waiting for process exit." };
    }

    return {
      success: true,
      summary: `Process ${input.pid} exited`,
      output: {
        exitCode: res.exitCode,
        stdout: res.stdout,
        stderr: res.stderr
      }
    };
  } catch (e: any) {
    return { success: false, summary: "Wait failed", error: e.message };
  }
}

async function handleWaitOutput(
  input: z.infer<typeof ProcessWaitOutputInputSchema>,
  context: ToolContext
): Promise<ToolResult<ProcessWaitOutput>> {
  if (!context.agent.state.ownedProcesses.includes(input.pid)) {
    return { success: false, summary: "Access denied", error: `Access denied or process ${input.pid} not found` };
  }
  const handle = Engine.processManager.get(input.pid);
  if (!handle) return { success: false, summary: "Not found", error: `Process ${input.pid} not found` };

  const pattern = new RegExp(input.regex);

  return new Promise((resolve) => {
    let timer: Timer;
    const check = (data: string) => {
      if (pattern.test(data)) {
        clearTimeout(timer);
        resolve({ success: true, summary: "Pattern matched", output: { match: data, message: `Match found for /${input.regex}/` } });
      }
    };

    handle.onOutput(check);

    if (pattern.test(handle.stdout) || pattern.test(handle.stderr)) {
      resolve({ success: true, summary: "Pattern matched (history)", output: { match: "history", message: `Match found for /${input.regex}/ (in history)` } });
      return;
    }

    timer = setTimeout(() => {
      resolve({ success: false, summary: "Wait timeout", error: `Timeout waiting for regex /${input.regex}/` });
    }, input.timeoutMs);
  });
}

export const ProcessWaitTool: ITool<ProcessWaitInput, ProcessWaitOutput> = {
  metadata: {
    name: "process_wait",
    description: "Wait for process events.",
    scope: ToolScope.Process
  },
  input: ProcessWaitInputSchema,
  execute: async (input: ProcessWaitInput, context: ToolContext): Promise<ToolResult<ProcessWaitOutput>> => {
    switch (input.operation) {
      case "exit":
        return handleWaitExit(input, context);
      case "output":
        return handleWaitOutput(input, context);
      default:
        return { success: false, summary: "Invalid operation", error: `Unknown operation: ${(input as any).operation}` };
    }
  },
  summarizeInput: (input: ProcessWaitInput) => {
    return `process_wait: ${input.operation}`;
  },
  summary: (output: ToolResult<ProcessWaitOutput>) => {
    return output.summary;
  }
};

export const AllProcessTools = [ProcessControlTool, ProcessExecuteTool, ProcessWaitTool];
