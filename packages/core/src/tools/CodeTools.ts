import { z } from "zod";
import type { ITool, ToolContext, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";
import * as crypto from "node:crypto";
import * as path from "node:path";

const CodeLanguageSchema = z.union([z.literal("python"), z.literal("node"), z.literal("bun")]);

export interface CodeExecuteInput {
  language: z.infer<typeof CodeLanguageSchema>;
  code: string;
  install?: string[];
}

export interface CodeExecuteOutput {
  exitCode: number;
  stdout: string;
  stderr: string;
}

const CodeExecuteInputSchema = z.object({
  language: CodeLanguageSchema,
  code: z.string(),
  install: z.array(z.string()).optional()
});

function getInterpreterCommand(language: string): { cmd: string; args: string[]; fileExt: string } {
  switch (language) {
    case "python":
      return { cmd: "python3", args: [], fileExt: ".py" };
    case "node":
      return { cmd: "node", args: [], fileExt: ".js" };
    case "bun":
      return { cmd: "bun", args: ["run"], fileExt: ".ts" };
    default:
      return { cmd: "python3", args: [], fileExt: ".py" };
  }
}

function buildInstallCommands(language: string, packages: string[]): string[] {
  if (packages.length === 0) return [];

  switch (language) {
    case "python":
      return ["pip", "install", "-q", ...packages];
    case "node":
      return ["npm", "install", "--silent", ...packages];
    case "bun":
      return ["bun", "add", ...packages];
    default:
      return [];
  }
}

export const CodeExecuteTool: ITool<CodeExecuteInput, CodeExecuteOutput> = {
  metadata: {
    name: "code_execute",
    description: "Execute code in a sandboxed environment.",
    scope: ToolScope.Process
  },
  input: CodeExecuteInputSchema,
  execute: async (input: CodeExecuteInput, context: ToolContext): Promise<ToolResult<CodeExecuteOutput>> => {
    try {
      const cwd = context.host.defaultCwd.toString();
      const tmpDir = path.join(cwd, "tmp");
      await context.host.mkdir(tmpDir).catch(() => {});

      const fileExt = getInterpreterCommand(input.language).fileExt;
      const fileName = `code_execute_${crypto.randomUUID()}${fileExt}`;
      const filePath = path.join(tmpDir, fileName);

      // Write the code to a temporary file
      await context.host.writeFile(filePath, input.code);

      // If packages need to be installed, run installer first
      if (input.install && input.install.length > 0) {
        const installCmd = buildInstallCommands(input.language, input.install);
        if (installCmd.length > 0) {
          const fullInstallCmd = [installCmd[0]!, ...installCmd.slice(1)].join(' ');
          const installResult = await context.host.exec(fullInstallCmd, {
            cwd: cwd,
            timeout: 300000 // 5 min for installs
          });

          if (installResult.exitCode !== 0) {
            // Cleanup code file
            await context.host.remove(filePath).catch(() => {});
            return {
              success: false,
              summary: "Package installation failed",
              error: `Installation failed (exit ${installResult.exitCode}): ${installResult.stderr}`,
              output: { exitCode: installResult.exitCode, stdout: installResult.stdout, stderr: installResult.stderr }
            };
          }
        }
      }

      // Execute the code
      const { cmd, args } = getInterpreterCommand(input.language);
      const fullCmd = [cmd, ...args, filePath].join(' ');
      const result = await context.host.exec(fullCmd, {
        cwd: cwd,
        timeout: 60000 // 60 second timeout
      });

      // Cleanup
      await context.host.remove(filePath).catch(() => {});

      return {
        success: result.exitCode === 0,
        summary: `Code executed (exit ${result.exitCode})`,
        output: {
          exitCode: result.exitCode,
          stdout: result.stdout,
          stderr: result.stderr,
        },
        error: result.exitCode === 0 ? undefined : result.stderr
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
  summarizeInput: (input: CodeExecuteInput) => {
    return `${input.language} code (${input.code.length} chars)`;
  },
  summary: (output: ToolResult<CodeExecuteOutput>) => {
    return output.summary;
  }
};

export const AllCodeTools = [CodeExecuteTool];
