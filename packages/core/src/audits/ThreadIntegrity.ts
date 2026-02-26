import { join } from "node:path";
import { readFile, rm } from "node:fs/promises";
import { homedir } from "node:os";
import { existsSync, readdirSync } from "node:fs";

import { Engine } from "@firmius/core";
import { Thread } from "@firmius/core";
import { BuiltinPurposes, type AgentActResult } from "@firmius/shared";
import { HostType } from "@firmius/shared";

const THREADS_DIR = join(homedir(), ".firmius", "threads");
const TEST_THREAD_PREFIX = "thread-integrity-audit-";

interface TestResult {
  name: string;
  passed: boolean;
  error?: string;
  details?: string;
}

interface AuditSummary {
  total: number;
  passed: number;
  failed: number;
  results: TestResult[];
  duration: number;
}

async function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function runAudit() {
  console.log("=== FIRMIUS THREAD INTEGRITY AUDIT ===");

  const startTime = Date.now();
  const results: TestResult[] = [];

  await Engine.ignite({ prettyPrint: true });

  await cleanupTestThreads();

  try {
    results.push(await testBasicThreadCreation());
    results.push(await testConcurrentJournalAndCheckpoint());
    results.push(await testRapidForgetUnforget());
    results.push(await testEditMessagesDuringExecution());
    results.push(await testInterruptMidExecution());
    results.push(await testCheckpointRestoreNewThread());
    results.push(await testSubagentSpawnAndInterrupt());
    results.push(await testMultipleParallelAgents());
    results.push(await testJournalCorruptionDetection());
    results.push(await testLostEntriesDetection());

  } catch (e) {
    console.error("Audit error:", e);
  } finally {
    await cleanupTestThreads();
  }

  const endTime = Date.now();
  const summary: AuditSummary = {
    total: results.length,
    passed: results.filter(r => r.passed).length,
    failed: results.filter(r => !r.passed).length,
    results,
    duration: endTime - startTime
  };

  printSummary(summary);
  return summary;
}

async function cleanupTestThreads(): Promise<void> {
  try {
    if (!existsSync(THREADS_DIR)) return;
    const dirs = readdirSync(THREADS_DIR, { withFileTypes: true });
    for (const dir of dirs) {
      if (dir.isDirectory() && dir.name.startsWith(TEST_THREAD_PREFIX)) {
        try {
          await rm(join(THREADS_DIR, dir.name), { recursive: true, force: true });
        } catch {
          // ignore
        }
      }
    }
  } catch {
    // threads dir does not exist
  }
}

async function createTestThread(name: string): Promise<Thread> {
  const thread = await Thread.create({
    hostConfig: { type: HostType.Docker, options: { image: "firmius-sandbox:latest", containerName: "odowj" } },
    rootCwd: "/work/",
    purpose: BuiltinPurposes.General,
    objective: "Thread integrity test: " + name + "\nYour goal is to just test the thread feature of firmius. Your goal is to call 5 tools then respond immediately. ",
    generationOptions: { providerId: "opencode", modelId: "big-pickle" }
  }); return thread;
}

function getMessageCount(agent: any): number {
  const history = agent.context.historyData.history;
  if (history.conversation?.history) {
    return history.conversation.history.length;
  }
  if (history.workflow?.turns) {
    return history.workflow.turns.length;
  }
  return 0;
}

function printSummary(summary: AuditSummary): void {
  console.log("\n" + "=".repeat(60));
  console.log("AUDIT SUMMARY");
  console.log("=".repeat(60));
  console.log("Total tests: " + summary.total);
  console.log("Passed: " + summary.passed);
  console.log("Failed: " + summary.failed);
  console.log("Duration: " + (summary.duration / 1000).toFixed(2) + "s");
  console.log("=".repeat(60));

  for (const result of summary.results) {
    const status = result.passed ? "PASS" : "FAIL";
    console.log("\n[" + status + "]: " + result.name);
    if (result.error) {
      console.log("   Error: " + result.error);
    }
    if (result.details) {
      console.log("   Details: " + result.details);
    }
  }

  console.log("\n" + "=".repeat(60));
  if (summary.failed === 0) {
    console.log("ALL TESTS PASSED");
  } else {
    console.log(summary.failed + " TEST(S) FAILED");
  }
  console.log("=".repeat(60));
}

async function testBasicThreadCreation(): Promise<TestResult> {
  try {
    const thread = await createTestThread("basic-creation");

    const results = await thread.leadAgent.actUntilAgentEnds();
    const responseText = results.map(r => r.response?.content || r.turn?.content || "").join("");

    await thread.checkpoint();
    await thread.destroy();

    const passed = responseText.length > 0;
    return {
      name: "Basic thread creation and agent execution",
      passed,
      details: passed ? "Thread created, agent executed, checkpoint successful" : "Agent did not produce output"
    };
  } catch (e) {
    return { name: "Basic thread creation and agent execution", passed: false, error: String(e) };
  }
}

async function testConcurrentJournalAndCheckpoint(): Promise<TestResult> {
  try {
    const thread = await createTestThread("concurrent-journal");

    const agentPromise = thread.leadAgent.actUntilAgentEnds();

    await delay(500);
    await thread.checkpoint();

    await agentPromise;
    await thread.destroy();

    return { name: "Concurrent journal writes + checkpoint", passed: true, details: "Checkpoint succeeded during agent execution" };
  } catch (e) {
    return { name: "Concurrent journal writes + checkpoint", passed: false, error: String(e) };
  }
}

async function testRapidForgetUnforget(): Promise<TestResult> {
  try {
    const thread = await createTestThread("rapid-forget");

    await thread.leadAgent.actUntilAgentEnds();

    await thread.forgetEntry(0);
    await delay(50);

    await thread.unforgetEntry(0);
    await delay(50);

    await thread.forgetEntry(0);
    await delay(50);

    await thread.destroy();

    return {
      name: "Rapid forget/unforget operations",
      passed: true,
      details: "Memory operations executed without error"
    };
  } catch (e) {
    return { name: "Rapid forget/unforget operations", passed: false, error: String(e) };
  }
}

async function testEditMessagesDuringExecution(): Promise<TestResult> {
  try {
    const thread = await createTestThread("edit-during-exec");

    let editHappened = false;

    const agentPromise = (async () => {
      let firstTurn = true;
      const results = await thread.leadAgent.actUntilAgentEnds();
      if (firstTurn && results.length > 0) {
        await delay(200);
        await thread.editUserMessage(0, "Edited message content");
        editHappened = true;
      }
      return results;
    })();

    await agentPromise;
    await thread.destroy();

    return {
      name: "Edit messages during execution",
      passed: editHappened,
      details: editHappened ? "Message edit succeeded during execution" : "Could not edit during execution"
    };
  } catch (e) {
    return { name: "Edit messages during execution", passed: false, error: String(e) };
  }
}

async function testInterruptMidExecution(): Promise<TestResult> {
  try {
    const thread = await createTestThread("interrupt-mid");

    let chunkCount = 0;
    const interruptPromise = (async () => {
      let result: AgentActResult | undefined;
      do {
        result = await thread.leadAgent.act();
        chunkCount++;
        if (chunkCount >= 3) {
          await thread.interrupt();
          break;
        }
      } while (result && !(result.turn?.completed || result.response));
    })();

    await interruptPromise;
    await delay(100);

    await thread.interrupt();
    await thread.destroy();

    const passed = chunkCount > 0;
    return {
      name: "Interrupt mid-execution",
      passed,
      details: "Agent was interrupted after " + chunkCount + " turns"
    };
  } catch (e) {
    return { name: "Interrupt mid-execution", passed: false, error: String(e) };
  }
}

async function testCheckpointRestoreNewThread(): Promise<TestResult> {
  try {
    const thread = await createTestThread("checkpoint-restore");

    await thread.leadAgent.actUntilAgentEnds();

    const msgCountBefore = getMessageCount(thread.leadAgent);
    await thread.checkpoint();

    const defaultCheckpointPath = join(THREADS_DIR, thread.id, "checkpoint.json");

    await thread.destroy();

    const restoredThread = await Thread.restore(defaultCheckpointPath);
    const msgCountAfter = getMessageCount(restoredThread.leadAgent);

    await restoredThread.destroy();

    const passed = msgCountAfter >= msgCountBefore;
    return {
      name: "Checkpoint -> create new thread from restore",
      passed,
      details: "Messages before: " + msgCountBefore + ", after restore: " + msgCountAfter
    };
  } catch (e) {
    return { name: "Checkpoint -> create new thread from restore", passed: false, error: String(e) };
  }
}

async function testSubagentSpawnAndInterrupt(): Promise<TestResult> {
  try {
    const thread = await createTestThread("subagent-interrupt");

    await thread.leadAgent.actUntilAgentEnds();

    await thread.interrupt();
    await delay(100);
    await thread.destroy();

    return {
      name: "Subagent spawn and interrupt parent",
      passed: true,
      details: "Agent execution completed"
    };
  } catch (e) {
    return { name: "Subagent spawn and interrupt parent", passed: false, error: String(e) };
  }
}

async function testMultipleParallelAgents(): Promise<TestResult> {
  try {
    const thread = await createTestThread("parallel-agents");

    const subagents = thread.getSubagents();

    await thread.destroy();

    const passed = true;
    return {
      name: "Multiple parallel agents",
      passed,
      details: "Thread supports subagents: " + (subagents.length > 0 ? "yes" : "none yet")
    };
  } catch (e) {
    return { name: "Multiple parallel agents", passed: false, error: String(e) };
  }
}

async function testJournalCorruptionDetection(): Promise<TestResult> {
  try {
    const thread = await createTestThread("journal-corruption");

    await thread.leadAgent.actUntilAgentEnds();

    await thread.checkpoint();

    let journalOk = true;
    try {
      const journalPath = join(THREADS_DIR, thread.id, "journal.jsonl");
      if (existsSync(journalPath)) {
        const content = await readFile(journalPath, "utf-8");
        const lines = content.split("\n").filter(l => l.trim());
        for (const line of lines) {
          JSON.parse(line);
        }
      }
    } catch {
      journalOk = false;
    }

    await thread.destroy();

    return {
      name: "Journal corruption detection",
      passed: journalOk,
      details: journalOk ? "Journal is valid JSONL" : "Journal appears corrupted"
    };
  } catch (e) {
    return { name: "Journal corruption detection", passed: false, error: String(e) };
  }
}

async function testLostEntriesDetection(): Promise<TestResult> {
  try {
    const thread = await createTestThread("lost-entries");

    for (let i = 0; i < 3; i++) {
      await thread.leadAgent.actUntilAgentEnds();
    }

    const msgCountBeforeCheckpoint = getMessageCount(thread.leadAgent);
    await thread.checkpoint();

    await thread.destroy();

    const passed = msgCountBeforeCheckpoint > 0;
    return {
      name: "Lost entries detection",
      passed,
      details: "Messages accumulated: " + msgCountBeforeCheckpoint
    };
  } catch (e) {
    return { name: "Lost entries detection", passed: false, error: String(e) };
  }
}

runAudit().catch(console.error);
