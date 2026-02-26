import { z } from "zod";
import type { ITool, ToolContext, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";

// =============================================================================
// GIT OPS TOOL
// =============================================================================

const GitOpsInputSchema = z.object({
  operation: z
    .enum([
      "status",
      "diff",
      "log",
      "commit",
      "branch",
      "checkout",
      "stash",
      "init",
    ])
    .describe("The git operation to perform."),
  args: z
    .array(z.string())
    .optional()
    .describe("Additional arguments to pass to the git command."),
  message: z
    .string()
    .optional()
    .describe("Commit message (required for commit operation)."),
  branch: z
    .string()
    .optional()
    .describe("Branch name (for checkout/branch operations)."),
});

interface GitOpsInput {
  operation:
    | "status"
    | "diff"
    | "log"
    | "commit"
    | "branch"
    | "checkout"
    | "stash"
    | "init";
  args?: string[];
  message?: string;
  branch?: string;
}

interface GitOpsOutput {
  output: string;
}

const runGitCommand = async (
  cwd: string,
  command: string[],
): Promise<{ success: boolean; output: string }> => {
  const proc = Bun.spawn(["git", ...command], {
    cwd,
    stdout: "pipe",
    stderr: "pipe",
  });

  const [stdout, stderr, exitCode] = await Promise.all([
    new Response(proc.stdout).text(),
    new Response(proc.stderr).text(),
    proc.exited,
  ]);

  if (exitCode !== 0) {
    return {
      success: false,
      output:
        stderr.trim() ||
        stdout.trim() ||
        `git command failed (exit ${exitCode})`,
    };
  }

  return {
    success: true,
    output: stdout.trim(),
  };
};

export const GitOpsTool: ITool<GitOpsInput, GitOpsOutput> = {
  metadata: {
    name: "git_ops",
    description: `Execute common git operations.`,
    scope: ToolScope.Git,
  },
  input: GitOpsInputSchema,
  execute: async (
    input: GitOpsInput,
    context: ToolContext,
  ): Promise<ToolResult<GitOpsOutput>> => {
    const cwd = String(context.agent.environment.cwd);

    const gitExists = await context.host.exists(".git");
    if (!gitExists) await context.host.exec("git init");

    try {
      let res: { success: boolean, output: string };
      switch (input.operation) {
        case "status":
          res = await runGitCommand(cwd, ["status", "--short"]);
          break;
        case "init":
          res = await runGitCommand(cwd, ["init"]);
          break;
        case "diff":
          res = await runGitCommand(cwd, ["diff", ...(input.args || [])]);
          break;
        case "log":
          res = await runGitCommand(cwd, ["log", ...(input.args || ["--oneline", "-20"])]);
          break;
        case "commit":
          res = await runGitCommand(cwd, ["commit", "-m", input.message || "Changes"]);
          break;
        case "branch":
          if (input.branch) res = await runGitCommand(cwd, ["branch", ...(input.args || []), input.branch]);
          else res = await runGitCommand(cwd, ["branch", ...(input.args || [])]);
          break;
        case "checkout":
          if (!input.branch && (!input.args || input.args.length === 0)) {
             return { success: false, summary: "Checkout failed", error: "Branch name required for checkout" };
          }
          res = await runGitCommand(cwd, ["checkout", ...(input.args || []), ...(input.branch ? [input.branch] : [])]);
          break;
        case "stash":
          res = await runGitCommand(cwd, ["stash", ...(input.args || ["list"])]);
          break;
        default:
          return { success: false, summary: "Invalid operation", error: `Unknown operation: ${input.operation}` };
      }

      return {
        success: res.success,
        summary: res.success ? `git ${input.operation} completed` : `git ${input.operation} failed`,
        output: { output: res.output },
        error: res.success ? undefined : res.output
      };
    } catch (error: any) {
      return {
        success: false,
        summary: `git ${input.operation} error`,
        error: error.message,
      };
    }
  },
  summarizeInput: (input: GitOpsInput) => {
    const extra = input.branch ? ` ${input.branch}` : "";
    return `git ${input.operation}${extra}`;
  },
  summary: (output: ToolResult<GitOpsOutput>) => {
    return output.summary;
  },
};

export const AllGitTools = [GitOpsTool];
