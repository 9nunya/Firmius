import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType, AgentActType } from "@firmius/shared";
import { HostType } from "@firmius/shared";
import { Thread } from "@firmius/core";
import type { AgentContentEvent, AgentTerminatedEvent, AgentThinkingEvent, ToolCallStartEvent } from "@firmius/shared";

// Configuration: max agent runtime before forcing exit
const MAX_AGENT_RUNTIME_MS = 30000; // 30 seconds total before forcing exit

interface TestMetrics {
  turnCount: number;
  finalResponseReceived: boolean;
  toolCallsAfterResponse: number;
  continuedAfterFinal: boolean;
  agentTerminated: boolean;
  terminationReason: string | null;
}

function withTimeout<T>(promise: Promise<T>, ms: number, label: string): Promise<T> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      reject(new Error(`Timeout after ${ms}ms: ${label}`));
    }, ms);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (error) => {
        clearTimeout(timer);
        reject(error);
      }
    );
  });
}

async function runConversationalTerminationTest() {
  console.log("=== CONVERSATIONAL TERMINATION AUDIT ===");
  console.log("Testing: Agent should properly terminate after responding in conversational mode");
  console.log(`Configuration: max_agent_runtime=${MAX_AGENT_RUNTIME_MS}ms`);
  console.log("");

  await Engine.ignite({ prettyPrint: true });

  const metrics: TestMetrics = {
    turnCount: 0,
    finalResponseReceived: false,
    toolCallsAfterResponse: 0,
    continuedAfterFinal: false,
    agentTerminated: false,
    terminationReason: null,
  };

  // Create a fresh thread with conversational mode
  const thread = await Thread.create({
    hostConfig: { type: HostType.Local },
    rootCwd: process.cwd(),
    purpose: BuiltinPurposes.General,
    objective: "You are a helpful assistant. Respond naturally to user messages. If the conversation is complete, call the complete_task tool with reason='task_complete'.",
    workType: AgentWorkType.Conversational,
    generationOptions: {
      providerId: "nanogpt",
      modelId: "openai/gpt-oss-120b",
      reasoningEffort: "medium",
    },
  });

  const leadAgent = thread.leadAgent;

console.log("Thread created. Sending initial user message...");

// Simulate user sending a message (adds to history)
const userMessage = "Hello! Can you tell me a fun fact about space?";
  const history = leadAgent.context!.historyData.history;
  if (history.type === AgentWorkType.Conversational && history.conversation) {
    history.conversation.history.push({
      isUser: true,
      content: userMessage,
      timestamp: Date.now(),
      tokens: 0,
      protected: false,
    });
  }

  // Subscribe to events to track behavior
  Engine.eventEmitter.on('agent_content', (event: AgentContentEvent) => {
    if (event.agentId === leadAgent.id) {
      console.log(`[Content stream] ${event.content.substring(0, 50)}...`);
    }
  });

  Engine.eventEmitter.on('agent_thinking', (event: AgentThinkingEvent) => {
    if (event.agentId === leadAgent.id) {
      console.log(`[Thinking] ${event.content.substring(0, 50)}...`);
    }
  });

  Engine.eventEmitter.on('tool_call_start', (event: ToolCallStartEvent) => {
    if (event.agentId === leadAgent.id) {
      console.log(`[Tool call start] ${event.toolName} - ${event.summary}`);
      // If we already got a final response but now seeing tool calls, that's a bug
      if (metrics.finalResponseReceived) {
        metrics.toolCallsAfterResponse++;
        console.error(`!!! BUG DETECTED: Tool call after final response! (count: ${metrics.toolCallsAfterResponse})`);
      }
    }
  });

  Engine.eventEmitter.on('agent_terminated', (event: AgentTerminatedEvent) => {
    if (event.agentId === leadAgent.id) {
      console.log(`[Agent terminated] reason: ${event.reason}, success: ${event.success}`);
      metrics.agentTerminated = true;
      metrics.terminationReason = event.reason;
    }
  });

  // Run the agent with a timeout to catch hangs
  try {
    const startTime = Date.now();
    console.log("Starting agent.actUntilAgentEnds() with timeout...");

    let results;
    try {
      results = await withTimeout(leadAgent.actUntilAgentEnds(), MAX_AGENT_RUNTIME_MS, "agent execution");
    } catch (timeoutErr: any) {
      if (timeoutErr.message.includes('Timeout')) {
        console.error(`\n❌ TIMEOUT: Agent did not terminate within ${MAX_AGENT_RUNTIME_MS}ms`);
        console.error("This indicates the agent is stuck in a loop or failed to return.");
        metrics.continuedAfterFinal = true;
        await thread.dispose();
        process.exit(1);
      }
      throw timeoutErr;
    }

    const duration = Date.now() - startTime;
    metrics.turnCount = results.length;
    const lastAction = results[results.length - 1];

    if (!lastAction) {
      console.error("❌ BUG: actUntilAgentEnds returned empty array!");
      await thread.dispose();
      process.exit(1);
    }

    if (lastAction.type === AgentActType.Response) {
      console.log("\n--- Response Received ---");
      const rawContent = lastAction.response?.content;
      const contentText = typeof rawContent === "string" ? rawContent : "(complex content)";
      console.log(`Content: ${contentText.substring(0, 150)}...`);
      metrics.finalResponseReceived = true;
      console.log(`Agent returned Response after ${duration}ms.`);
    } else if (lastAction.type === AgentActType.Turn) {
      console.log("\n--- Turn Received (has tool calls) ---");
      const toolNames = lastAction.turn?.toolCalls.map(tc => tc.name).join(', ') || 'none';
      console.log(`Tool calls: ${toolNames}`);
    }

    // Check: For conversational agents, a Response should be returned (no further turns)
    if (lastAction.type === AgentActType.Response && history.type === AgentWorkType.Conversational) {
      console.log("\n✅ CORRECT: Agent returned Response for conversational message.");
    } else if (history.type === AgentWorkType.Conversational && lastAction.type === AgentActType.Turn) {
      console.error("\n❌ BUG: Conversational agent returned Turn instead of Response. It should respond directly without requiring tool calls.");
      metrics.continuedAfterFinal = true;
    }

    // Wait a bit to see if any events fire after the return (unlikely but possible if background tasks)
    await new Promise(resolve => setTimeout(resolve, 1000));

    // Force cleanup
    await thread.dispose();

    console.log("\n=== TEST SUMMARY ===");
    console.log(`Work type: ${AgentWorkType[history.type]}`);
    console.log(`Total duration: ${duration}ms`);
    console.log(`Turns executed: ${metrics.turnCount}`);
    console.log(`Final response received: ${metrics.finalResponseReceived}`);
    console.log(`Tool calls after response: ${metrics.toolCallsAfterResponse}`);
    console.log(`Agent terminated: ${metrics.agentTerminated}`);
    if (metrics.terminationReason) console.log(`Termination reason: ${metrics.terminationReason}`);

    if (metrics.toolCallsAfterResponse > 0) {
      console.error("\n❌ AUDIT FAILED: Tool calls occurred after final response (agent should have terminated).");
      process.exit(1);
    }
    if (metrics.continuedAfterFinal) {
      console.error("\n❌ AUDIT FAILED: Agent continued after final response or timed out.");
      process.exit(1);
    }
    if (!metrics.agentTerminated && lastAction.type !== AgentActType.Response) {
      console.warn("\n⚠️  WARNING: Agent did not explicitly terminate; may rely on process exit.");
    }

    console.log("\n✅ AUDIT PASSED: Agent behaved correctly for conversational termination.");
    process.exit(0);

  } catch (e: any) {
    console.error("\n❌ Test failed with exception:", e.message || e);
    if (e instanceof Error) console.error(e.stack);
    process.exit(1);
  }
}

// Execute the audit
runConversationalTerminationTest().catch(err => {
  console.error("Fatal error in audit:", err);
  process.exit(1);
});
