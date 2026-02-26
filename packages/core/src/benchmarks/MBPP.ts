import { Engine } from "../Engine";
import { BuiltinPurposes } from "@firmius/shared";
import type { AgentToolResult } from "@firmius/shared";
import { DockerHost } from "../hosts/DockerHost";
import path from "node:path";
import os from "node:os";
import fs from "node:fs";

const CACHE_DIR =
  process.env.MBPP_CACHE || path.join(os.homedir(), ".firmius/cache/mbpp");
const CACHE_DATASET_PATH = path.join(CACHE_DIR, "sanitized-mbpp.json");

interface MBPPProblem {
  task_id: number;
  prompt: string;
  code: string;
  test_imports: string[];
  test_list: string[];
  source_file?: string;
}

async function ensureDatasetCached(): Promise<void> {
  if (!fs.existsSync(CACHE_DIR)) {
    fs.mkdirSync(CACHE_DIR, { recursive: true });
  }

  if (!fs.existsSync(CACHE_DATASET_PATH)) {
    console.log(
      `Downloading MBPP sanitized dataset to ${CACHE_DATASET_PATH}...`,
    );
    const res = await fetch(
      "https://raw.githubusercontent.com/google-research/google-research/refs/heads/master/mbpp/sanitized-mbpp.json",
    );
    if (!res.ok) throw new Error(`Failed to download dataset: ${res.status}`);
    const data = await res.arrayBuffer();
    fs.writeFileSync(CACHE_DATASET_PATH, Buffer.from(data));
    console.log("Dataset cached.");
  }
}

async function loadProblems(): Promise<MBPPProblem[]> {
  const raw = fs.readFileSync(CACHE_DATASET_PATH, "utf-8");
  const problems = JSON.parse(raw) as MBPPProblem[];
  console.log(`Loaded ${problems.length} MBPP problems.`);
  return problems;
}

function extractFunctionName(code: string): string {
  const match = code.match(/def\s+(\w+)\s*\(/);
  if (!match || !match[1]) {
    throw new Error(
      `Could not extract function name from code:\n${code.substring(0, 200)}...`,
    );
  }
  return match[1];
}

function generateRunTestsScript(): string {
  return `import json
import sys
from importlib.util import spec_from_file_location, module_from_spec

def main():
    with open('tests.json', 'r') as f:
        data = json.load(f)

    entry_point = data['entry_point']
    test_imports = data.get('test_imports', [])
    test_list = data['test_list']

    # Load solution
    spec = spec_from_file_location('solution', 'solution.py')
    solution = module_from_spec(spec)

    # Prepare globals with imports
    globals_dict = {}
    for imp in test_imports:
        try:
            exec(imp, globals_dict)
        except Exception as e:
            print(f"Import failed: {imp} -> {e}", file=sys.stderr)

    spec.loader.exec_module(solution)

    if not hasattr(solution, entry_point):
        print(json.dumps({'passed': 0, 'total': len(test_list), 'error': 'Missing function'}))
        sys.exit(0)

    func = getattr(solution, entry_point)
    globals_dict[entry_point] = func

    passed = 0
    for test in test_list:
        try:
            exec(test, globals_dict)
            passed += 1
        except AssertionError:
            pass
        except Exception:
            pass

    print(json.dumps({'passed': passed, 'total': len(test_list)}))

if __name__ == '__main__':
    main()
`;
}

async function runBenchmark() {
  console.log("=== MBPP Runner for Firmius ===");
  console.log(`Cache dir: ${CACHE_DIR}`);

  await ensureDatasetCached();
  const problems = await loadProblems();
  if (problems.length === 0) {
    console.error("No problems found.");
    return;
  }

  const problem = problems[Math.floor(Math.random() * problems.length)]!;
  const entryPoint = extractFunctionName(problem.code);
  console.log(`\nSelected problem #${problem.task_id}: ${problem.prompt}`);
  console.log(`Function to implement: ${entryPoint}`);

  const containerName = `firmius-mbpp-${Math.random().toString(36).substring(7)}`;

  await Engine.ignite({ prettyPrint: true });

  const host = new DockerHost({
    containerName,
    image: "firmius-sandbox:latest",
    env: {},
    volumes: {},
  });

  await host.init();

  try {
    // Prepare workspace
    await host.exec("mkdir -p /work/mbpp");

    // Write files
    const testsJson = JSON.stringify(
      {
        entry_point: entryPoint,
        test_imports: problem.test_imports,
        test_list: problem.test_list,
      },
      null,
      2,
    );
    await host.writeFile("/work/mbpp/tests.json", testsJson);
    await host.writeFile("/work/mbpp/run_tests.py", generateRunTestsScript());
    await host.writeFile("/work/mbpp/solution.py", ""); // empty file

    console.log("\nSummoning agent...");
    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Coder,
      objective: `Implement the function \`${entryPoint}\` in /work/mbpp/solution.py.\n\nProblem: ${problem.prompt}\n\nAfter implementing, run /work/mbpp/run_tests.py to verify. When all tests pass, call complete_task with summary "PASS". If you cannot pass all tests, call complete_task with reason "task_impossible" and an appropriate summary.`,
      cwd: "/work/mbpp",
      host,
    });

    console.log("Agent starting autonomous loop...");
    const actions = await agent.actUntilAgentEnds();
    console.log(`Agent finished after ${actions.length} turn(s).`);

    // Run tests ourselves
    console.log("\nRunning tests to evaluate...");
    const testRes = await host.exec("python3 /work/mbpp/run_tests.py", {
      cwd: "/work/mbpp",
      timeout: 10000,
    });
    if (testRes.exitCode !== 0) {
      console.error("Test script error:", testRes.stderr);
      return;
    }

    let testResult: { passed: number; total: number };
    try {
      testResult = JSON.parse(testRes.stdout.trim());
    } catch (e) {
      console.error("Failed to parse test result:", testRes.stdout);
      return;
    }

    const { passed, total } = testResult;
    const success = passed === total;
    console.log(`Tests: ${passed}/${total} passed.`);

    // Check agent's termination summary if available
    let agentSummary: string | null = null;
    for (const act of actions) {
      if (act.turn) {
        for (const tc of act.turn.toolCalls) {
          if (tc.name === "complete_task") {
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
        const rawContent = act.response.content;
        agentSummary = typeof rawContent === "string" ? rawContent.trim() : JSON.stringify(rawContent);
        break;
      }
    }

    if (agentSummary) {
      console.log(`Agent summary: "${agentSummary}"`);
    }

    console.log(`\n=== RESULT: ${success ? "SUCCESS" : "FAILURE"} ===`);

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
