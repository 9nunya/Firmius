import { Engine } from "../Engine";
import { BuiltinPurposes } from "@firmius/shared";
import { DockerHost } from "../hosts/DockerHost";
import path from "node:path";
import os from "node:os";
import fs from "node:fs";
import { execSync } from "node:child_process";
import { logger } from "@firmius/shared";
import type { HostProcessResult } from "@firmius/shared";

const SWE_DATASET_MODE: "VERIFIED" | "PRO" = "PRO";
const REPO_CACHE_DIR =
  process.env.SWE_REPO_CACHE ||
  path.join(os.homedir(), ".firmius/swebench-temp-repos");
const PARALLEL_SETUP = process.env.SWE_PARALLEL_SETUP !== "false";

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
  before_repo_set_cmd?: string;
  patch?: string;
  test_patch?: string;
}

interface SetupResult {
  setupHost: DockerHost;
  containerName: string;
  instance: SWEBenchInstance;
  beforeResults: { passed: string[]; failed: string[]; errors: string[] };
  testCmd: string;
  language: string;
  setupTime: number;
  envVars: Record<string, string>;
}

function normalizeInstanceFields(instance: any): SWEBenchInstance {
  const normalized = { ...instance } as SWEBenchInstance;

  if (instance.fail_to_pass && !instance.FAIL_TO_PASS) {
    normalized.FAIL_TO_PASS = instance.fail_to_pass;
  }
  if (instance.pass_to_pass && !instance.PASS_TO_PASS) {
    normalized.PASS_TO_PASS = instance.pass_to_pass;
  }

  return normalized;
}

async function fetchRandomSWEBenchInstance(): Promise<SWEBenchInstance> {
  const isPro = SWE_DATASET_MODE === "PRO";
  const datasetName = isPro
    ? "ScaleAI%2FSWE-bench_Pro"
    : "princeton-nlp%2FSWE-bench_Verified";

  console.log(
    `Fetching random SWE-bench ${isPro ? "Pro" : "Verified"} instance...`,
  );

  const infoRes = await fetch(
    `https://datasets-server.huggingface.co/info?dataset=${datasetName}&config=default`,
  );
  if (!infoRes.ok)
    throw new Error(`Failed to fetch dataset info: ${infoRes.status}`);
  const info = (await infoRes.json()) as {
    dataset_info: { splits: { name: string; num_examples: number }[] };
  };

  const split = info.dataset_info.splits[0];
  const rowCount = split?.num_examples || 100;

  const res = await fetch(
    `https://datasets-server.huggingface.co/rows?dataset=${datasetName}&config=default&split=test&offset=0&length=${rowCount}`,
  );
  if (!res.ok) throw new Error(`Failed to fetch dataset: ${res.status}`);
  const data = (await res.json()) as { rows: { row: any }[] };
  const instances = data.rows.map((r) => normalizeInstanceFields(r.row));
  const randomIndex = Math.floor(Math.random() * instances.length);
  const instance = instances[randomIndex]!;
  console.log(
    `Selected instance: ${instance.instance_id} (${instance.repo}) [${instance.repo_language || "python"}]`,
  );
  return instance;
}

function parseTests(failToPass: string): string[] {
  let tests: string[] = [];
  try {
    tests = JSON.parse(failToPass) as string[];
  } catch (e) {
    const cleaned = failToPass.trim();
    const quoted = cleaned.startsWith(`'`) && cleaned.endsWith(`'`);
    if (quoted) {
      try {
        tests = JSON.parse(cleaned.slice(1, -1)) as string[];
      } catch (e2) {
        tests = cleaned
          .split(`',`)
          .map((t) => t.trim().replace(/^[\"\`']|[\"\`']$/g, ""));
      }
    } else if (cleaned.startsWith(`[`) && cleaned.endsWith(`]`)) {
      try {
        tests = JSON.parse(cleaned) as string[];
      } catch (e2) {
        tests = cleaned
          .slice(1, -1)
          .split(`,`)
          .map((t) => t.trim().replace(/^[\"\`']|[\"\`']$/g, ""));
      }
    } else if (cleaned.startsWith(`"`)) {
      try {
        tests = JSON.parse(cleaned) as string[];
      } catch (e2) {
        tests = cleaned
          .slice(1, -1)
          .split(`,`)
          .map((t) => t.trim().replace(/^[\"\`']|[\"\`']$/g, ""));
      }
    } else {
      tests = cleaned.split(`,`).map((t) => t.trim());
    }
  }
  return tests.filter((t) => t.length > 0);
}

function buildTestCommand(
  repo: string,
  failToPass: string,
  repoLanguage?: string,
): string {
  const tests = parseTests(failToPass);
  const language = repoLanguage || "python";

  if (language === "go") {
    // Go tests use -run flag with regex pattern
    // SWE-bench format: "TestName" or "TestName | file.go" or "TestName/Subtest"
    const testPatterns = tests
      .map((t) => {
        // Extract test name, handling "name | file" format
        const match = t.match(/^([^|]+)/);
        if (match && match[1]) {
          return match[1].trim();
        }
        return t.trim();
      })
      .filter((t) => t);

    if (testPatterns.length === 0) {
      return "go test -v ./...";
    }

    // Join tests with | for regex OR pattern
    const runPattern = testPatterns.join("|");
    return `go test -v -run "${runPattern}" ./...`;
  }

  if (language === "js" || language === "ts") {
    // Parse format: 'test/file.js | Test description' or just 'test/file.js'
    const testConfigs = tests
      .map((t) => {
        const match = t.match(/^(.+?)\s*\|\s*(.+)$/);
        if (match && match[1] && match[2]) {
          return { file: match[1].trim(), description: match[2].trim() };
        }
        // No pipe separator, treat entire string as file
        return { file: t.trim(), description: null };
      })
      .filter((t) => t.file);

    if (testConfigs.length === 0) {
      return "npm test";
    }

    // Group by file and use grep for test descriptions
    const files = [...new Set(testConfigs.map((t) => t.file))];
    const grepPattern = testConfigs
      .filter((t) => t.description)
      .map((t) => t.description!.replace(/[()]/g, "\\$&")) // Escape parens for grep
      .join("|");

    if (files.length === 1 && grepPattern) {
      // Single file with grep pattern
      return `npm test ${files[0]} -- --grep "${grepPattern}"`;
    } else if (grepPattern) {
      // Multiple files or just grep across all
      return `npm test -- --grep "${grepPattern}"`;
    } else {
      // Just run specific files
      return `npm test ${files.join(" ")}`;
    }
  }

  if (repo.includes("django")) {
    const labels = tests.map((test) => {
      const legacyMatch = test.match(/^(.+?)\s*\((.+?)\)$/);
      if (legacyMatch && legacyMatch[1] && legacyMatch[2]) {
        return `${legacyMatch[2]}.${legacyMatch[1]}`;
      }
      return test;
    });
    return `python3 /work/repo/tests/runtests.py ${labels.join(" ")} --parallel=1 --noinput`;
  }

  const convertedTests = tests.map((test) => {
    const legacyMatch = test.match(/^(.+?)\s*\((.+?)\)$/);
    if (legacyMatch && legacyMatch[1] && legacyMatch[2]) {
      return `-k "${legacyMatch[1]}"`;
    }
    return test;
  });

  const testNames = convertedTests
    .filter((t) => t.startsWith("-k "))
    .map((t) => t.replace("-k ", ""))
    .join(" or ");
  if (testNames) {
    return `python3 -m pytest -k "${testNames}" -v`;
  }

  return `python3 -m pytest ${convertedTests.join(" ")} -v`;
}

function parseTestResults(output: string): {
  passed: string[];
  failed: string[];
  errors: string[];
} {
  const results = {
    passed: [] as string[],
    failed: [] as string[],
    errors: [] as string[],
  };
  const lines = output.split("\n");
  for (const line of lines) {
    if (
      line.includes("ModuleNotFoundError:") ||
      line.includes("SyntaxError:")
    ) {
      results.errors.push(line.trim());
    }

    // Mocha/Jest specific patterns (more strict to avoid false positives)
    // Mocha: "✓ Test description" or "✔ Test description"
    const jsPassed = line.match(/^\s*[✓✔]\s+(.+)$/);
    // Mocha: "✗ Test description" or "✖ Test description" or "1) Test description"
    const jsFailed =
      line.match(/^\s*[✗✖]\s+(.+)$/) || line.match(/^\s*\d+\)\s+(.+)$/);

    if (jsPassed && jsPassed[1]) results.passed.push(jsPassed[1].trim());
    if (jsFailed && jsFailed[1]) results.failed.push(jsFailed[1].trim());

    // Mocha summary lines: "X passing (Yms)" or "X failing"
    const mochaPassing = line.match(/(\d+)\s+passing/);
    const mochaFailing = line.match(/(\d+)\s+failing/);
    if (mochaPassing) results.passed.push(`mocha_${mochaPassing[1]}_passing`);
    if (mochaFailing) results.failed.push(`mocha_${mochaFailing[1]}_failing`);

    const pyPassed = line.match(/(\S+)\s+PASSED/);
    const pyFailed = line.match(/(\S+)\s+FAILED/);
    const pyError = line.match(/^ERROR\s+(\S+)/) || line.match(/(\S+)\s+ERROR/);

    if (pyPassed && pyPassed[1]) results.passed.push(pyPassed[1]);
    if (pyFailed && pyFailed[1]) results.failed.push(pyFailed[1]);
    if (pyError && pyError[1]) results.errors.push(pyError[1]);

    if (line.includes("FAILED (failures="))
      results.failed.push("django_failure");
    if (line.includes("FAILED (errors=")) results.errors.push("django_error");

    const djangoFail = line.match(/^FAIL:\s+(\S+)/);
    const djangoError = line.match(/^ERROR:\s+(\S+)/);
    if (djangoFail && djangoFail[1]) results.failed.push(djangoFail[1]);
    if (djangoError && djangoError[1]) results.errors.push(djangoError[1]);

    if (line.trim() === "OK") results.passed.push("django_ok");

    // Go test format: --- PASS: TestName (0.00s) or --- FAIL: TestName (0.00s)
    const goPass = line.match(/^---\s+PASS:\s+(\S+)/);
    const goFail = line.match(/^---\s+FAIL:\s+(\S+)/);

    if (goPass && goPass[1]) results.passed.push(goPass[1]);
    if (goFail && goFail[1]) results.failed.push(goFail[1]);
    // Skipped tests don't count as passed or failed

    // Go test summary: PASS or FAIL
    if (line.trim() === "PASS") results.passed.push("go_pass_summary");
    if (line.trim() === "FAIL") results.failed.push("go_fail_summary");
  }

  return {
    passed: [...new Set(results.passed)],
    failed: [...new Set(results.failed)],
    errors: [...new Set(results.errors)],
  };
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
    execSync(`git clone https://github.com/${repo}.git ${repoCachePath}`, {
      stdio: "inherit",
    });
  } catch (e: any) {
    execSync(`rm -rf ${repoCachePath}`, { stdio: "inherit" });
    throw new Error(`Failed to clone/cache repo: ${e.message}`);
  }

  return repoCachePath;
}

async function setupEnvironmentParallel(
  instance: SWEBenchInstance,
  containerName: string,
): Promise<SetupResult> {
  const startTime = Date.now();
  const language = instance.repo_language || "python";

  const envVars: Record<string, string> = {};
  if (language === "python") {
    envVars.PYTHONPATH = "/work/repo";
    envVars.DJANGO_SETTINGS_MODULE = "tests.test_sqlite";
  }

  const repoCachePath = await ensureRepoCached(instance.repo);

  const setupHost = new DockerHost({
    containerName,
    image: "firmius-sandbox:latest",
    env: envVars,
    volumes: {
      [repoCachePath]: "/swe-cache:ro",
    },
  });

  await setupHost.init();

  try {
    console.log("Setting up repo from cache...");
    await setupHost.exec(`mkdir -p /work/repo`);

    const copyRes = await setupHost.exec(`cp -r /swe-cache/. /work/repo/`);
    if (copyRes.exitCode !== 0) {
      console.log("Cache copy failed, falling back to git clone...");
      const cloneRes = await setupHost.exec(
        `git clone https://github.com/${instance.repo}.git /work/repo`,
      );
      if (cloneRes.exitCode !== 0) {
        throw new Error(`Clone failed: ${cloneRes.stderr}`);
      }

      const checkoutRes = await setupHost.exec(
        `git checkout ${instance.base_commit}`,
        { cwd: "/work/repo" },
      );
      if (checkoutRes.exitCode !== 0)
        throw new Error(`Checkout failed: ${checkoutRes.stderr}`);
    }

    const setupTasks: Promise<any>[] = [];

    if (instance.before_repo_set_cmd) {
      console.log("Executing before_repo_set_cmd...");
      setupTasks.push(
        setupHost
          .exec(instance.before_repo_set_cmd, { cwd: "/work/repo" })
          .then((res: HostProcessResult) => {
            if (res.exitCode !== 0) {
              console.error("Setup cmd failed:", res.stderr);
            }
          }),
      );
    }

    console.log("Installing environment...");
    if (language === "python") {
      setupTasks.push(
        setupHost
          .exec("pip install -e .", { cwd: "/work/repo", timeout: 120000 })
          .then((res: HostProcessResult) => {
            if (res.exitCode !== 0) {
              logger.debug(
                `[SWE-Bench] pip install -e . failed: ${res.stderr}`,
              );
            }
          }),
      );
      // Skip common packages since they're in the pre-built image
    } else if (language === "go") {
      setupTasks.push(
        Promise.all([
          setupHost.exec("go mod download", {
            cwd: "/work/repo",
            timeout: 120000,
          }),
          setupHost.exec("go mod tidy", { cwd: "/work/repo", timeout: 120000 }),
        ]),
      );
    } else if (language === "js" || language === "ts") {
      setupTasks.push(
        setupHost
          .exec("npm install", { cwd: "/work/repo", timeout: 180000 })
          .then((res: HostProcessResult) => {
            if (res.exitCode !== 0) {
              logger.debug(`[SWE-Bench] npm install failed: ${res.stderr}`);
            }
          }),
      );
    }

    // Wait for all setup tasks to complete
    await Promise.all(setupTasks);

    const testCmd = buildTestCommand(
      instance.repo,
      instance.FAIL_TO_PASS,
      language,
    );
    console.log(`Running baseline tests: ${testCmd}`);
    const beforeRes = await setupHost.exec(testCmd, {
      cwd: "/work/repo",
      timeout: 120000,
    });
    const beforeResults = parseTestResults(beforeRes.stdout + beforeRes.stderr);
    console.log(
      `Baseline: ${beforeResults.passed.length} passed, ${beforeResults.failed.length} failed, ${beforeResults.errors.length} errors`,
    );

    const setupTime = (Date.now() - startTime) / 1000;
    console.log(`Setup completed in ${setupTime.toFixed(1)}s`);

    return {
      setupHost,
      containerName,
      instance,
      beforeResults,
      testCmd,
      language,
      setupTime,
      envVars,
    };
  } catch (e) {
    await setupHost.destroy();
    throw e;
  }
}

async function runBenchmark() {
  console.log("=== SWE-Bench Runner for Firmius (Optimized) ===");
  console.log(`Repo cache: ${REPO_CACHE_DIR}`);
  console.log(`Parallel setup: ${PARALLEL_SETUP}`);

  const instance = await fetchRandomSWEBenchInstance();
  const containerName = `firmius-swe-${Math.random().toString(36).substring(7)}`;

  await Engine.ignite({ prettyPrint: true });

  let setupHost: DockerHost | undefined;

  try {
    // Optimized parallel setup with caching
    const setupResult = await setupEnvironmentParallel(instance, containerName);
    setupHost = setupResult.setupHost;
    const { beforeResults, testCmd, instance: inst } = setupResult;

    console.log(`Summoning agent with container: ${containerName}...`);

    let contextHints = inst.hints_text || "";
    if (inst.requirements && SWE_DATASET_MODE === "PRO") {
      contextHints += `\n\nRequirements:\n${inst.requirements}`;
    }
    if (inst.interface && SWE_DATASET_MODE === "PRO") {
      contextHints += `\n\nInterface:\n${inst.interface}`;
    }

    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Coder,
      objective: `You are tasked with fixing a bug in a repository located at /work/repo.

Problem Statement:
${inst.problem_statement}
${contextHints}

Failing Tests:
${inst.FAIL_TO_PASS}

Instructions:
1. Explore the codebase to locate the issue.
2. Modify the code to fix the bug.
3. Verify your fix using the provided tests.

IMPORTANT: This repository uses a CUSTOM testing framework. The failing tests indicate the actual test framework and test files to run. Read the test files to understand how tests are structured and run. Do NOT assume standard pytest unless the test files clearly show pytest is used. Use the appropriate test runner for this project (e.g., custom scripts, specific test commands, or the framework indicated in the test files).`,
      cwd: "/work/repo",
      host: setupHost,
    });

    // Agent budget is managed internally - starting autonomous loop
    console.log("Agent starting autonomous loop...");
    await agent.actUntilAgentEnds();
    console.log("Agent finished task.");

    console.log("Running tests AFTER fix...");
    const afterRes = await setupHost.exec(testCmd, {
      cwd: "/work/repo",
      timeout: 120000,
    });
    const afterResults = parseTestResults(afterRes.stdout + afterRes.stderr);
    console.log(
      `Final: ${afterResults.passed.length} passed, ${afterResults.failed.length} failed, ${afterResults.errors.length} errors`,
    );

    const fixedTests = beforeResults.failed.filter((test) =>
      afterResults.passed.includes(test),
    );
    const fixedErrors = beforeResults.errors.filter((test) =>
      afterResults.passed.includes(test),
    );

    const errorCountDecreased =
      afterResults.errors.length < beforeResults.errors.length;
    const passCountIncreased =
      afterResults.passed.length > beforeResults.passed.length;
    const failCountDecreased =
      afterResults.failed.length < beforeResults.failed.length;

    const hasImprovement =
      fixedTests.length > 0 ||
      fixedErrors.length > 0 ||
      errorCountDecreased ||
      passCountIncreased ||
      failCountDecreased;

    if (hasImprovement) {
      console.log(`SUCCESS: Agent made improvements:`);
      if (fixedTests.length > 0)
        console.log(`  - Fixed ${fixedTests.length} failed test(s)`);
      if (fixedErrors.length > 0)
        console.log(`  - Fixed ${fixedErrors.length} error(s)`);
      if (errorCountDecreased)
        console.log(
          `  - Errors decreased: ${beforeResults.errors.length} → ${afterResults.errors.length}`,
        );
      if (passCountIncreased)
        console.log(
          `  - Passes increased: ${beforeResults.passed.length} → ${afterResults.passed.length}`,
        );
      if (failCountDecreased)
        console.log(
          `  - Failures decreased: ${beforeResults.failed.length} → ${afterResults.failed.length}`,
        );
    } else {
      console.log("FAILURE: No improvements detected.");
      console.log(
        `  Before: ${beforeResults.passed.length} passed, ${beforeResults.failed.length} failed, ${beforeResults.errors.length} errors`,
      );
      console.log(
        `  After: ${afterResults.passed.length} passed, ${afterResults.failed.length} failed, ${afterResults.errors.length} errors`,
      );
    }

    await Engine.agentFactory.terminate(agent.id);
  } catch (e) {
    console.error("Benchmark failed:", e);
  } finally {
    console.log("Cleaning up container...");
    if (setupHost) {
      try {
        await setupHost.destroy();
      } catch (cleanupError) {
        console.error("Failed to cleanup container:", cleanupError);
      }
    }
  }
}

if (import.meta.main) {
  runBenchmark();
}
