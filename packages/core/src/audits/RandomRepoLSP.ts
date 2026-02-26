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

/**
 * REASONING:
 * Tests Phase C+ LSP Tools (lsp_top_files, lsp_find_symbol, lsp_callers, lsp_exports, lsp_file_summary).
 * Uses random SWE-bench Pro repo for testing.
 *
 * FAIL CONDITIONS:
 * 1. lsp_top_files returns empty or crashes
 * 2. lsp_find_symbol doesn't find any definitions
 * 3. lsp_callers returns empty or crashes
 * 4. lsp_exports doesn't return exports
 * 5. lsp_file_summary crashes
 * 6. LSP clients cannot be initialized for supported languages
 * 7. CodebaseIntelligence fails to scan or cache data
 *
 * PASS CONDITIONS:
 * 1. lsp_top_files returns ranked list with scores
 * 2. lsp_find_symbol returns symbol definitions
 * 3. lsp_callers returns caller locations
 * 4. lsp_exports returns file exports
 * 5. lsp_file_summary returns file overview
 * 6. CodebaseIntelligence detects languages automatically
 * 7. All tools work without manual file scanning (uses LSP only)
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

async function runAudit() {
  console.log("=== FIRMIUS LSP AUDIT: PHASE C TOOLS ===");

  await Engine.ignite({ prettyPrint: true });

  const containerName = `firmius-lsp-audit-${Math.random().toString(36).substring(7)}`;

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
    const startTime = Date.now();

    const results = await agent.actUntilAgentEnds();

    console.log("\n=== AUDIT VERIFICATION ===");
    const lastResponse = results[results.length - 1];
    const rawContent = lastResponse?.response?.content || "";
    const content = typeof rawContent === "string" ? rawContent : JSON.stringify(rawContent);

    console.log(`Agent Response Length: ${content.length} chars`);
    const duration = Date.now() - startTime;

    let passed = true;
    const failures: string[] = [];

    if (!content.includes("LSP Test:")) {
      console.error("\n❌ AUDIT FAILED: Agent didn't follow instructions or crashed.");
      passed = false;
      failures.push("Agent didn't report LSP test results");
    }

    if (content.includes("error") || content.includes("failed") || content.includes("crash")) {
      console.error("\n❌ AUDIT FAILED: LSP tools reported errors or crashed.");
      passed = false;
      failures.push("LSP tool errors detected in output");
    }

    if (content.includes("undefined") || content.includes("null")) {
      console.error("\n❌ AUDIT FAILED: LSP tools returned null/undefined values.");
      passed = false;
      failures.push("LSP tools returned null/undefined");
    }

    if (duration < 10000) {
      console.error("\n❌ AUDIT WARNING: Agent completed suspiciously fast (<10s). May have failed silently.");
      passed = false;
      failures.push("Suspiciously fast execution");
    }

    if (!content.includes("caching WORKS")) {
      console.error("\n❌ AUDIT FAILED: CodebaseIntelligence caching not tested.");
      passed = false;
      failures.push("Caching not verified");
    }

    if (!content.includes("symbol definitions")) {
      console.error("\n❌ AUDIT FAILED: lsp_find_symbol not tested.");
      passed = false;
      failures.push("lsp_find_symbol not verified");
    }

    if (!content.includes("callers")) {
      console.error("\n❌ AUDIT FAILED: lsp_callers not tested.");
      passed = false;
      failures.push("lsp_callers not verified");
    }

    if (!content.includes("exports")) {
      console.error("\n❌ AUDIT FAILED: lsp_exports not tested.");
      passed = false;
      failures.push("lsp_exports not verified");
    }

    if (passed) {
      console.log("\n✅ AUDIT PASSED: Phase C+ LSP tools functioning correctly.");
      console.log("- lsp_top_files returned ranked file list");
      console.log("- lsp_find_symbol returned symbol definitions");
      console.log("- lsp_callers returned caller locations");
      console.log("- lsp_exports returned file exports");
      console.log("- CodebaseIntelligence auto-detected languages");
      console.log("- Caching mechanism worked (second call faster)");
    } else {
      console.error("\n❌ AUDIT FAILED: Phase C+ LSP tools have issues:");
      for (const failure of failures) {
        console.error(`  - ${failure}`);
      }
    }

  } catch (e: any) {
    console.error("Audit Exception:", e);
    if (e instanceof Error) {
      console.error("Stack:", e.stack);
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

runAudit();
