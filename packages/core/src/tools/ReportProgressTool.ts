import { z } from "zod";
import type { ITool, ToolContext, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";

const ReportProgressInputSchema = z.object({
  progress: z.string().describe("A brief description of current progress."),
});

type ReportProgressInput = z.infer<typeof ReportProgressInputSchema>;

interface ReportProgressOutput {
  message: string;
}

export const ReportProgressTool: ITool<
  ReportProgressInput,
  ReportProgressOutput
> = {
  metadata: {
    name: "report_progress",
    description:
      "Report progress on your current work.",
    scope: ToolScope.Worker,
  },
  input: ReportProgressInputSchema,
  execute: async (
    input: ReportProgressInput,
    context: ToolContext,
  ): Promise<ToolResult<ReportProgressOutput>> => {
    const coordinator = context.coordinator;
    const agentId = context.agent.identity.id;

    try {
      await coordinator.fleet.updateProgressTimestamp(agentId);
      return {
        success: true,
        summary: `Progress: ${input.progress}`,
        output: { message: `Progress reported: ${input.progress}` },
      };
    } catch (error: any) {
      return {
        success: false,
        summary: "Failed to report progress",
        error: error.message,
      };
    }
  },
  summarizeInput: (input: ReportProgressInput) =>
    `progress: ${input.progress.slice(0, 50)}...`,
  summary: (output: ToolResult<ReportProgressOutput>) => output.summary,
};
