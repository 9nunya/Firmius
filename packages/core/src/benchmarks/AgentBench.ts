import { Engine } from "../Engine";
import { BuiltinPurposes } from "@firmius/shared";
import type { AgentToolResult } from "@firmius/shared";
import { DockerHost } from "../hosts/DockerHost";
import path from "node:path";
import os from "node:os";
import fs from "node:fs";

const CACHE_DIR = process.env.AGENTBENCH_CACHE || path.join(os.homedir(), ".firmius/cache/agentbench");
const CACHE_DATASET_PATH = path.join(CACHE_DIR, "dev.json");

interface AgentBenchTask {
  description: string;
  create?: {
    local?: string;
    init?: { code: string };
  };
  start?: string;
  evaluation: {
    match?: string;
    example?: { code: string } | { file: string };
    check?: any;
  };
  labels?: string[];
}

async function ensureDatasetCached(): Promise<void> {
  if (!fs.existsSync(CACHE_DIR)) {
    fs.mkdirSync(CACHE_DIR, { recursive: true });
  }

  if (!fs.existsSync(CACHE_DATASET_PATH)) {
    console.log(`Downloading AgentBench dev dataset to ${CACHE_DATASET_PATH}...`);
    const res = await fetch('https://raw.githubusercontent.com/THUDM/AgentBench/main/data/os_interaction/data/dev.json');
    if (!res.ok) throw new Error(`Failed to download dataset: ${res.status}`);
    const data = await res.arrayBuffer();
    fs.writeFileSync(CACHE_DATASET_PATH, Buffer.from(data));
    console.log("Dataset cached.");
  }
}

async function loadTasks(): Promise<AgentBenchTask[]> {
  const raw = fs.readFileSync(CACHE_DATASET_PATH, 'utf-8');
  const tasks = JSON.parse(raw) as AgentBenchTask[];

  // Filter to tasks we can run:
  // - Must have evaluation (match OR example.code)
  // - Should not have 'start' field (background processes are complex)
  const runnable = tasks.filter(t => {
    if (!t.evaluation) return false;
    const hasMatch = !!t.evaluation.match;
    const hasExampleCode = t.evaluation.example && typeof t.evaluation.example === 'object' && 'code' in t.evaluation.example;
    return hasMatch || hasExampleCode;
  });

  console.log(`Loaded ${runnable.length} runnable tasks out of ${tasks.length} total.`);

  return runnable;
}

async function computeExampleOutput(host: DockerHost, code: string): Promise<string> {
  const res = await host.exec(code, { cwd: '/root', timeout: 10000 });
  if (res.exitCode !== 0) {
    throw new Error(`Example code failed: ${res.stderr}`);
  }
  return res.stdout.trim();
}

async function runBenchmark() {
  console.log("=== AgentBench Runner for Firmius ===");
  console.log(`Cache dir: ${CACHE_DIR}`);

  await ensureDatasetCached();
  const tasks = await loadTasks();
  if (tasks.length === 0) {
    console.error("No runnable tasks found.");
    return;
  }

  const task = tasks[Math.floor(Math.random() * tasks.length)]!;
  console.log(`\nSelected task description: ${task.description}`);

  const containerName = `firmius-agentbench-${Math.random().toString(36).substring(7)}`;

  await Engine.ignite({ prettyPrint: true });

  const host = new DockerHost({
    containerName,
    image: 'firmius-sandbox:latest',
    env: {},
    volumes: {}
  });

  await host.init();

  try {
    // Setup task environment
    if (task.create?.init?.code) {
      console.log("Executing task init script...");
      const initRes = await host.exec(task.create.init.code, { cwd: '/root' });
      if (initRes.exitCode !== 0) {
        console.warn(`Init script returned non-zero: ${initRes.exitCode}\n${initRes.stderr}`);
      }
    }

    // Determine expected answer
    let expectedAnswer: string | null = null;
    let computeExpectedInContainer = false;
    if (task.evaluation.match) {
      expectedAnswer = task.evaluation.match;
      console.log(`Expected answer (direct): "${expectedAnswer}"`);
    } else if (task.evaluation.example && typeof task.evaluation.example === 'object' && 'code' in task.evaluation.example) {
      computeExpectedInContainer = true;
      console.log("Will compute expected answer from example code in container.");
    }

    console.log("\nSummoning agent...");
    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Coder,
      objective: `${task.description}\n\nWhen you have determined the answer, call complete_task with the answer in the summary field. Do not output anything after calling complete_task.`,
      cwd: "/root",
      host,
      constraints: { allowOutsideCwd: true },
      generationOptions: {
        providerId: "nanogpt",
        modelId: "moonshotai/kimi-k2.5"
      }
    });

    console.log("Agent starting autonomous loop...");
    const actions = await agent.actUntilAgentEnds();
    console.log(`Agent finished after ${actions.length} turn(s).`);

     // Extract agent summary from complete_task tool result or final response
     let agentSummary: string | null = null;
     for (const act of actions) {
       if (act.turn) {
         // Case 1: complete_task tool was called in this turn; result is in toolResults
         for (const tc of act.turn.toolCalls) {
           if (tc.name === 'complete_task') {
              const tr = act.turn.toolResults.find((r: AgentToolResult) => r.id === tc.id);
             if (tr) {
               const result = tr.result as any;
               if (result?.summary) {
                 agentSummary = result.summary;
               }
             }
             break;
           }
         }
         if (agentSummary) break;
        } else if (act.response?.content) {
          // Case 2: final response may contain summary if termination was immediate
          const rawContent = act.response.content;
          agentSummary = typeof rawContent === "string" ? rawContent.trim() : JSON.stringify(rawContent);
          break;
       }
     }
 
     if (!agentSummary) {
       console.error("Agent did not call complete_task with a summary.");
       await Engine.agentFactory.terminate(agent.id);
       return;
     }

    console.log(`Agent answer: "${agentSummary}"`);

    // Compute expected if needed
    let finalExpected: string;
    if (computeExpectedInContainer) {
      const exampleCode = (task.evaluation.example as { code: string }).code;
      finalExpected = await computeExampleOutput(host, exampleCode);
      console.log(`Computed expected from example: "${finalExpected}"`);
    } else {
      finalExpected = expectedAnswer!;
    }

    const success = agentSummary.trim() === finalExpected.trim();
    console.log(`\n=== RESULT: ${success ? "SUCCESS" : "FAILURE"} ===`);
    console.log(`Agent: "${agentSummary.trim()}"`);
    console.log(`Expected: "${finalExpected.trim()}"`);

    await Engine.agentFactory.terminate(agent.id);

  } catch (e) {
    console.error("Benchmark failed:", e);
  } finally {
    console.log("Cleaning up container...");
    await host.destroy();
  }
}

if (import.meta.main) {
  runBenchmark();
}
