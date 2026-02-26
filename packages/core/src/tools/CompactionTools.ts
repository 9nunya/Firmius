import { z } from "zod";
import type { ITool, ToolContext, AgentTurn, AgentConversationalMessage, AgentWorkflow, ToolResult } from "@firmius/shared/types";
import { ToolScope, AgentWorkType, isAgentWorkflow } from "@firmius/shared";
import { Engine } from "@firmius/core";

export type CompactionOperation = "inspect" | "read" | "compress";

export interface InspectCompactionOutput {
    turns: { index: number; role: string; tokens: number; summaryAvailable: boolean }[];
    totalTokens: number;
}

export interface ReadCompactionOutput {
    details: { index: number; content: unknown }[];
}

export interface CompressCompactionOutput {
    message: string;
}

export type CompactionManageOutput = InspectCompactionOutput | ReadCompactionOutput | CompressCompactionOutput;

const BaseCompactionManageInput = z.object({
    operation: z.enum(["inspect", "read", "compress"]).describe("The compaction management operation to perform.")
});

const InspectCompactionInput = BaseCompactionManageInput.extend({
    operation: z.literal("inspect")
});

const ReadCompactionInput = BaseCompactionManageInput.extend({
    operation: z.literal("read"),
    indices: z.array(z.number()).describe("The turn indices to read.")
});

const CompressCompactionInput = BaseCompactionManageInput.extend({
    operation: z.literal("compress"),
    indices: z.array(z.number()).describe("Indices of turns to compress."),
    summary: z.string().describe("A high-density semantic synthesis of the events in these turns.")
});

const CompactionManageInput = z.discriminatedUnion("operation", [
    InspectCompactionInput,
    ReadCompactionInput,
    CompressCompactionInput
]);

type CompactionManageInputType = z.infer<typeof CompactionManageInput>;

function getTargetAgent(context: ToolContext) {
    const targetId = context.agent.execution.tags["targetAgentId"];
    if (!targetId) throw new Error("No targetAgentId found in agent tags.");

    const target = Engine.agentFactory.agents.get(targetId);
    if (!target) throw new Error(`Target agent ${targetId} not found.`);

    return target;
}

function executeInspect(context: ToolContext): ToolResult<InspectCompactionOutput> {
    const target = getTargetAgent(context);
    const history = target.context?.historyData?.history;
    if (!history) return { success: true, summary: "No history found", output: { turns: [], totalTokens: 0 } };
    
    let turns: { index: number; role: string; tokens: number; summaryAvailable: boolean }[] = [];

    if (history.type === AgentWorkType.Goal && history.workflow && isAgentWorkflow(history.workflow)) {
        turns = history.workflow.turns.map((t: AgentTurn, i: number) => ({
            index: i,
            role: "assistant",
            tokens: t.tokens,
            summaryAvailable: !!t.summary
        }));
    } else if (history.type === AgentWorkType.Conversational && history.conversation) {
        turns = history.conversation.history.map((m: AgentConversationalMessage | AgentWorkflow, i: number) => {
            if (isAgentWorkflow(m)) {
                return {
                    index: i,
                    role: "workflow",
                    tokens: m.turns.reduce((acc: number, t: { tokens?: number }) => acc + (t.tokens || 0), 0),
                    summaryAvailable: m.turns.every((t: { summary?: string }) => !!t.summary)
                };
            } else {
                return {
                    index: i,
                    role: m.isUser ? "user" : "assistant",
                    tokens: (m as { tokens?: number }).tokens || 0,
                    summaryAvailable: false
                };
            }
        });
    }

    return {
        success: true,
        summary: `Inspected ${turns.length} turns`,
        output: {
            turns,
            totalTokens: target.context?.state?.metrics?.totalTokens ?? 0
        }
    };
}

function executeRead(indices: number[], context: ToolContext): ToolResult<ReadCompactionOutput> {
    const target = getTargetAgent(context);
    const history = target.context?.historyData?.history;

    if (!history) return { success: true, summary: "No history found", output: { details: [] } };

    const details = indices.map(i => {
        let content: unknown;
        if (history.type === AgentWorkType.Goal && history.workflow) {
            content = history.workflow.turns[i];
        } else if (history.type === AgentWorkType.Conversational && history.conversation) {
            content = history.conversation.history[i];
        }
        if (!content) throw new Error(`Turn index ${i} out of bounds.`);
        return { index: i, content };
    });

    return {
        success: true,
        summary: `Read ${details.length} turns`,
        output: { details }
    };
}

function executeCompress(indices: number[], summaryText: string, context: ToolContext): ToolResult<CompressCompactionOutput> {
    const target = getTargetAgent(context);
    const history = target?.context?.historyData?.history;

    if (!history) return { success: false, summary: "No history found", error: "No history found" };

    const sortedIndices = [...indices].sort((a, b) => a - b);
    let compressed = 0;
    let skippedProtected = 0;

    sortedIndices.forEach(idx => {
        if (history.type === AgentWorkType.Goal && history.workflow) {
            const t = history.workflow.turns[idx]!;
            if (t.protected) {
                skippedProtected++;
                return;
            }
            t.summary = summaryText;
            t.toolResults = [];
            compressed++;
        } else if (history.type === AgentWorkType.Conversational && history.conversation) {
            const m = history.conversation.history[idx]!;
            if (isAgentWorkflow(m)) {
                const protectedInWorkflow = m.turns.some(t => t.protected);
                if (protectedInWorkflow) {
                    skippedProtected++;
                    return;
                }
                m.turns.forEach(t => {
                    t.summary = summaryText;
                    t.toolResults = [];
                });
                compressed++;
            } else {
                (m as { summary?: string }).summary = summaryText;
                compressed++;
            }
        }
    });

    return {
        success: true,
        summary: `Compressed ${compressed} turns`,
        output: { message: `Compressed ${compressed} turns. Skipped ${skippedProtected} protected turns.` }
    };
}

export const CompactionManageTool: ITool<CompactionManageInputType, CompactionManageOutput> = {
    metadata: {
        name: "compaction_manage",
        description: "Manage agent context compaction.",
        scope: ToolScope.Compaction
    },
    input: CompactionManageInput,
    execute: async (input: CompactionManageInputType, context: ToolContext): Promise<ToolResult<CompactionManageOutput>> => {
        try {
            switch (input.operation) {
                case "inspect":
                    return executeInspect(context);
                case "read":
                    return executeRead(input.indices, context);
                case "compress":
                    return executeCompress(input.indices, input.summary, context);
                default:
                    return { success: false, summary: "Invalid op", error: `Unknown operation: ${(input as any).operation}` };
            }
        } catch (e: any) {
            return { success: false, summary: "Compaction failed", error: e.message };
        }
    },
    summarizeInput: (input: CompactionManageInputType): string => {
        return `compaction: ${input.operation}`;
    },
};

export const AllCompactionTools = [CompactionManageTool];
