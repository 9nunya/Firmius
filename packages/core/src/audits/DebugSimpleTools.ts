import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType } from "@firmius/shared";
import { LocalHost } from "@firmius/core/hosts";

// Thinking loop detection configuration
const THINKING_LOOP_CONFIG = {
  maxConsecutiveThinkingEvents: 20,
  maxThinkingTimeMs: 60000,
  minContentBetweenThinking: 10,
};

class ThinkingLoopDetector {
  private consecutiveThinkingEvents = 0;
  private lastToolCallTime = Date.now();
  private totalThinkingChars = 0;
  private isLoopDetected = false;
  private loopReason = "";

  reset() {
    this.consecutiveThinkingEvents = 0;
    this.lastToolCallTime = Date.now();
    this.totalThinkingChars = 0;
    this.isLoopDetected = false;
    this.loopReason = "";
  }

  onThinking(text: string) {
    this.consecutiveThinkingEvents++;
    this.totalThinkingChars += text.length;
    
    const timeSinceLastToolCall = Date.now() - this.lastToolCallTime;
    
    if (this.consecutiveThinkingEvents > THINKING_LOOP_CONFIG.maxConsecutiveThinkingEvents) {
      this.isLoopDetected = true;
      this.loopReason = `Too many consecutive thinking events (${this.consecutiveThinkingEvents} > ${THINKING_LOOP_CONFIG.maxConsecutiveThinkingEvents})`;
    }
    
    if (timeSinceLastToolCall > THINKING_LOOP_CONFIG.maxThinkingTimeMs) {
      this.isLoopDetected = true;
      this.loopReason = `Thinking for too long (${Math.round(timeSinceLastToolCall / 1000)}s > ${THINKING_LOOP_CONFIG.maxThinkingTimeMs / 1000}s)`;
    }
  }

  onToolCall() {
    this.consecutiveThinkingEvents = 0;
    this.lastToolCallTime = Date.now();
    this.totalThinkingChars = 0;
  }

  onContent(_text: string) {
  }

  hasLoopDetected(): boolean {
    return this.isLoopDetected;
  }

  getLoopReason(): string {
    return this.loopReason;
  }

  getStats() {
    return {
      consecutiveThinkingEvents: this.consecutiveThinkingEvents,
      timeSinceLastToolCall: Date.now() - this.lastToolCallTime,
      totalThinkingChars: this.totalThinkingChars,
    };
  }
}

async function runSimpleTest() {
  console.log("=== SIMPLE TOOL CALL TEST ===");
  console.log(`Configuration: ${JSON.stringify(THINKING_LOOP_CONFIG, null, 2)}`);
  console.log("");

  await Engine.ignite({ prettyPrint: true });

  const loopDetector = new ThinkingLoopDetector();

  Engine.eventEmitter.on('agent_thinking', (event) => {
    loopDetector.onThinking(event.content);
    
    if (loopDetector.hasLoopDetected()) {
      console.error("\n" + "=".repeat(60));
      console.error("🔄 THINKING LOOP DETECTED!");
      console.error(`Reason: ${loopDetector.getLoopReason()}`);
      console.error("=".repeat(60));
      console.error("\nStats at time of detection:");
      console.error(JSON.stringify(loopDetector.getStats(), null, 2));
      console.error("\nAuto-exiting...");
      process.exit(1);
    }
  });

  Engine.eventEmitter.on('tool_call_start', () => {
    loopDetector.onToolCall();
  });

  Engine.eventEmitter.on('agent_content', (event) => {
    loopDetector.onContent(event.content);
  });

  try {
    const host = new LocalHost();
    await host.init();

    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.General,
      objective: `
You are testing a simple sequence of tool calls. Complete these steps in order:

Step 1: Use todo_add to create 3 todos: "Task A", "Task B", "Task C"
Step 2: Use todo_list to see all todos
Step 3: Use todo_update to mark "Task A" as completed
Step 4: Use todo_list again to verify the update
Step 5: Use complete_task with message "Simple test completed: Added 3 todos, marked 1 complete"

IMPORTANT: You must use the tools. Do not just think about it - actually call the tools.
      `.trim(),
      cwd: "/tmp",
      host: host,
      workType: AgentWorkType.Goal,
      generationOptions: {
        providerId: "nanogpt",
        modelId: "moonshotai/kimi-k2.5:thinking",
        reasoningEffort: "high",
        maxTokens: 4096,
      }
    });

    console.log("Starting simple tool call test...");
    console.log("Thinking loop detection is ACTIVE - will auto-exit if loop detected\n");
    
    const startTime = Date.now();

    const results = await agent.actUntilAgentEnds();

    console.log("\n=== TEST RESULTS ===");
    console.log(`Execution completed without thinking loop detection`);
    console.log(`Duration: ${Math.round((Date.now() - startTime) / 1000)}s`);
    console.log(`Final stats: ${JSON.stringify(loopDetector.getStats(), null, 2)}`);

    const lastResponse = results[results.length - 1];
    const rawContent = lastResponse?.response?.content || "";
    const content = typeof rawContent === "string" ? rawContent : JSON.stringify(rawContent);
    
    console.log(`\nAgent response: ${content.substring(0, 200)}...`);
    console.log("\n✅ Test completed successfully - no thinking loop detected");

  } catch (e: any) {
    if (loopDetector.hasLoopDetected()) {
      console.error("\n❌ THINKING LOOP WAS DETECTED - Process terminated");
    } else {
      console.error("Test Exception:", e);
      if (e instanceof Error) {
        console.error("Stack:", e.stack);
      }
    }
  }
}

runSimpleTest();
