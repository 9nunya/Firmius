import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType } from "@firmius/shared";
import path from "node:path";
import os from "node:os";
import fs from "node:fs";
import { execSync } from "node:child_process";
import { DockerHost } from "@firmius/core/hosts";

interface SWEBenchInstance {
  instance_id: string;
  repo: string;
  base_commit: string;
  problem_statement: string;
  hints_text?: string;
  requirements?: string;
  interface?: string;
  FAIL_TO_PASS: string;
  PASS_TO_PASS: string;
  repo_language?: string;
}

const REPO_CACHE_DIR = process.env.SWE_REPO_CACHE || path.join(os.homedir(), ".firmius/swebench-temp-repos");

// Thinking loop detection configuration
const THINKING_LOOP_CONFIG = {
  // Max thinking events without a tool call before detecting a loop
  maxConsecutiveThinkingEvents: 20,
  // Max time spent thinking without a tool call (in ms)
  maxThinkingTimeMs: 60000,
  // Min content length that must be produced before allowing more thinking
  minContentBetweenThinking: 10,
};

/**
 * REASONING:
 * Debug script to test for thinking loops with kimi-k2.5:thinking model.
 * Auto-exits when a thinking loop is detected.
 */

async function fetchRandomSWEBenchInstance(): Promise<SWEBenchInstance> {
  const datasetName = 'ScaleAI%2FSWE-bench_Pro';

  console.log(`Fetching random SWE-bench Pro instance...`);

  const infoRes = await fetch(
    `https://datasets-server.huggingface.co/info?dataset=${datasetName}&config=default`
  );
  if (!infoRes.ok) throw new Error(`Failed to fetch dataset info: ${infoRes.status}`);
  const info = await infoRes.json() as { dataset_info: { splits: { name: string; num_examples: number }[] } };

  const split = info.dataset_info.splits[0];
  const rowCount = split?.num_examples || 100;

  const res = await fetch(
    `https://datasets-server.huggingface.co/rows?dataset=${datasetName}&config=default&split=test&offset=0&length=${rowCount}`
  );
  if (!res.ok) throw new Error(`Failed to fetch dataset: ${res.status}`);
  const data = await res.json() as { rows: { row: any }[] };
  const instances: SWEBenchInstance[] = data.rows.map(r => ({ ...r.row }));
  const randomIndex = Math.floor(Math.random() * instances.length);
  const instance = instances[randomIndex]!;
  console.log(`Selected instance: ${instance.instance_id} (${instance.repo}) [${instance.repo_language || "python"}]`);
  return instance;
}

async function ensureRepoCached(repo: string): Promise<string> {
  const cacheDir = REPO_CACHE_DIR;

  if (!fs.existsSync(cacheDir)) {
    fs.mkdirSync(cacheDir, { recursive: true });
  }

  const repoCachePath = path.join(cacheDir, repo);

  if (fs.existsSync(repoCachePath)) {
    console.log(`Using cached repo: ${repoCachePath}`);
    return repoCachePath;
  }

  console.log(`Cloning and caching repo: ${repoCachePath}`);

  try {
    execSync(`git clone https://github.com/${repo}.git ${repoCachePath}`, { stdio: "inherit" });
  } catch (e: any) {
    execSync(`rm -rf ${repoCachePath}`, { stdio: "inherit" });
    throw new Error(`Failed to clone/cache repo: ${e.message}`);
  }

  return repoCachePath;
}

class ThinkingLoopDetector {
  private consecutiveThinkingEvents = 0;
  private lastToolCallTime = Date.now();
  private totalThinkingChars = 0;
  private isLoopDetected = false;
  private loopReason = "";

  constructor() {
    this.reset();
  }

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
    
    // Check for thinking loop conditions
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

async function runDebugAudit() {
  console.log("=== FIRMIUS DEBUG: THINKING LOOP DETECTION ===");
  console.log(`Configuration: ${JSON.stringify(THINKING_LOOP_CONFIG, null, 2)}`);
  console.log("");

  await Engine.ignite({ prettyPrint: true });

  const containerName = `firmius-debug-thinking-${Math.random().toString(36).substring(7)}`;
  const loopDetector = new ThinkingLoopDetector();

  // Set up event listeners for thinking loop detection
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
    const instance = await fetchRandomSWEBenchInstance();
    const repoCachePath = await ensureRepoCached(instance.repo);

    console.log("Setting up repo..")
    const host = new DockerHost({
      image: "firmius-sandbox:latest",
      containerName,
      volumes: {
        [repoCachePath]: "/work/repo"
      }
    });
    await host.init();
    await host.exec(`git checkout ${instance.base_commit}`, { cwd: "/work/repo" })

    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Researcher,
      objective: `
AUDIT MISSION: Test Phase C+ LSP tools

Step 1: Use lsp_top_files(limit=10) to get most important files
- Verify it returns files with symbolCount, referenceCount, and score
- Check that languages are auto-detected (no language parameter needed)

Step 2: Analyze top file using lsp_symbols()
- Get symbols from the most important file
- Verify symbols include classes, functions, methods

Step 3: Use lsp_find_symbol() to search for a symbol
- Search for a common symbol name (e.g., "Component", "Service", "Handler")
- Verify it returns definition locations

Step 4: Use lsp_callers() on a function
- Find a function from step 2
- Use lsp_callers to find who calls it

Step 5: Use lsp_exports() on top file
- Get exports from the top file
- Verify it returns functions, classes, interfaces

Step 6: Use lsp_file_summary() for quick overview
- Get file summary for top file
- Verify it returns imports, exports, classes, functions

Final message must summarize: "LSP Test: Found X top files, Y symbol definitions, Z callers, W exports, caching WORKS"
      `.trim(),
      cwd: "/work/repo",
      host: host,
      workType: AgentWorkType.Goal,
       generationOptions: {
         providerId: "nanogpt",
         modelId: "moonshotai/kimi-k2.5:thinking",
         reasoningEffort: "high",
       }
    });

    console.log("Starting CodebaseIntelligence scan (Phase C test)...");
    console.log("Thinking loop detection is ACTIVE - will auto-exit if loop detected\n");
    
    const startTime = Date.now();

    const results = await agent.actUntilAgentEnds();

    console.log("\n=== DEBUG RESULTS ===");
    console.log(`Execution completed without thinking loop detection`);
    console.log(`Duration: ${Math.round((Date.now() - startTime) / 1000)}s`);
    console.log(`Final stats: ${JSON.stringify(loopDetector.getStats(), null, 2)}`);

    const lastResponse = results[results.length - 1];
    const content = lastResponse?.response?.content || "";
    
    console.log(`\nAgent response length: ${content.length} chars`);
    console.log("\n✅ Debug completed successfully - no thinking loop detected");

  } catch (e: any) {
    if (loopDetector.hasLoopDetected()) {
      console.error("\n❌ THINKING LOOP WAS DETECTED - Process terminated");
    } else {
      console.error("Audit Exception:", e);
      if (e instanceof Error) {
        console.error("Stack:", e.stack);
      }
    }
   } finally {
     console.log("\nCleaning up...");
     try {
       execSync(`docker rm -f ${containerName}`, { stdio: "inherit" });
     } catch (e) {
       console.error("Cleanup error:", e);
     }
   }
}

runDebugAudit();
