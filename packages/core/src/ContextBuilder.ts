import {
  AgentWorkType,
  type AgentContext,
  type AgentTurn,
  type AgentConversationalMessage,
  type AgentWorkflow,
} from "@firmius/shared";
import {
  createMessageWithStringContent,
  type ProviderMessage,
} from "@firmius/shared";
import { FIRMIUS_SYSTEM_PROMPT } from "./SystemPrompt";
import type { BudgetTracker } from "./budget/BudgetTracker";
import type { CheckpointData } from "./budget/Types";
import { ContextCheckpoint } from "./budget/ContextCheckpoint";

export class ContextBuilder {
  static async createSystemPrompt(
    context: AgentContext,
    budgetTracker: BudgetTracker,
  ): Promise<string> {
    const allocation = budgetTracker.getAllocation();

    let promptInjection = "";

    promptInjection += `<budget>`;
    promptInjection += `<file_budget>${allocation.fileBudget} tokens</file_budget>`;
    promptInjection += `<protected_budget>${allocation.protectedBudget} tokens</protected_budget>`;
    promptInjection += `<rolling_budget>${allocation.rollingBudget} tokens</rolling_budget>`;
    promptInjection += `<current_usage>${budgetTracker.getLastPromptTokens()} tokens</current_usage>`;
    promptInjection += `</budget>`;

    const anchorsSection =
      context.execution.anchors.size > 0
        ? `\n\n## TASK ANCHORS (Immutable Decisions)\n${Array.from(
          context.execution.anchors,
        )
          .map((a) => `- ${a}`)
          .join("\n")}\n`
        : "";

    const injectedContext = context.execution.injectedContext || "";
    promptInjection =
      anchorsSection + "\n" + injectedContext + "\n" + promptInjection;

    const systemAdvisories =
      context.historyData.history.type === AgentWorkType.Goal
        ? `- **TERMINATING**: Write \`>>>DONE<<<\` when finished. Everything before it is shown to the user.
       - **THINKING**: Content without the marker means you're still working. The loop continues.
       - **ACTION ORIENTED**: Every turn should advance the objective through tool calls or analysis.
       - **VERIFICATION**: Always verify before marking done.`
        : `- **TERMINATING**: Write \`>>>DONE<<<\` when ready to send your message to the user.
       - **THINKING**: Your content is visible during execution but only finalized when you mark done.
       - **ENGAGEMENT**: Be natural and direct. Mark done when your response is complete.`;

    let finalPrompt = FIRMIUS_SYSTEM_PROMPT({
      ...context,
      execution: {
        ...context.execution,
        injectedContext: promptInjection,
      },
    });
    finalPrompt = finalPrompt.replace(
      /<system_advisories>[\s\S]*?<\/system_advisories>/g,
      `<system_advisories>\n${systemAdvisories}\n</system_advisories>`,
    );
    return finalPrompt;
  }



  private static turnToMessages(turn: AgentTurn): ProviderMessage[] {
    const messages: ProviderMessage[] = [];

    const assistantMsg: ProviderMessage = {
      role: "assistant",
      content: turn.content || "",
    };

    if (turn.reasoning) {
      (assistantMsg as any).reasoning = turn.reasoning;
    }

    if (turn.toolCalls && turn.toolCalls.length > 0) {
      assistantMsg.tool_calls = turn.toolCalls;
    }

    messages.push(assistantMsg);

    if (turn.toolResults && turn.toolResults.length > 0) {
      for (const result of turn.toolResults) {
        const tr = result.result;
        const data = (tr && typeof tr === 'object' && 'output' in tr) ? tr.output : tr;
        
        const resultContent =
          typeof data === "string"
            ? data
            : JSON.stringify(data ?? "");

        messages.push({
          role: "tool",
          tool_call_id: result.id,
          content: resultContent,
        });
      }
    }

    return messages;
  }

  private static conversationEntryToMessages(
    entry: AgentConversationalMessage | AgentWorkflow,
  ): ProviderMessage[] {
    const messages: ProviderMessage[] = [];

    if ("isUser" in entry) {
      const convMsg = entry as AgentConversationalMessage;
      messages.push({
        role: convMsg.isUser ? "user" : "assistant",
        content: convMsg.content,
      });

      if (!convMsg.isUser && convMsg.reasoning) {
        (messages[messages.length - 1] as any).reasoning = convMsg.reasoning;
      }
    } else {
      const workflow = entry as AgentWorkflow;
      for (const turn of workflow.turns) {
        messages.push(...this.turnToMessages(turn));
      }
    }

    return messages;
  }

  static async context2ProviderMessages(
    context: AgentContext,
    budgetTracker: BudgetTracker,
    checkpoints: CheckpointData[],
  ): Promise<ProviderMessage[]> {
    const messages: ProviderMessage[] = [];

    messages.push(
      createMessageWithStringContent(
        "system",
        await this.createSystemPrompt(context, budgetTracker),
      ),
    );

    if (context.historyData.history.type === AgentWorkType.Conversational) {
      const conv = context.historyData.history.conversation;
      if (conv) {
        for (const entry of conv.history) {
          messages.push(...this.conversationEntryToMessages(entry));
        }
      }
    } else if (context.historyData.history.type === AgentWorkType.Goal) {
      if (context.historyData.history.conversation) {
        for (const entry of context.historyData.history.conversation.history) {
          if ("content" in entry) {
            messages.push({
              role: "user",
              content: entry.content,
            });
          }
        }
      }

      const workflow = context.historyData.history.workflow;
      if (workflow) {
        for (const turn of workflow.turns) {
          messages.push(...this.turnToMessages(turn));
        }
      }
    }



    if (checkpoints.length > 0) {
      const checkpointMessages =
        ContextCheckpoint.toProviderMessage(checkpoints);
      messages.push(...checkpointMessages);
    }

    // Ensure there is at least one user message to prevent provider errors
    const hasUserMessage = messages.some(m => m.role === 'user');
    if (!hasUserMessage) {
      // Prioritize the agent's objective if no conversational history exists
      const objective = context.identity.objective || "Proceed.";
      messages.push(createMessageWithStringContent("user", objective));
    }

    return messages;
  }
}
