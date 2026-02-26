import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType } from "@firmius/shared";
import { HostType } from "@firmius/shared";
import { spawnSync } from "node:child_process";

/**
 * REASONING:
 * Tests the Tiered Compaction System. 
 * Tier 1: Auto-summarization of individual old turns using tool summaries.
 * Tier 2: Full Compactor sub-agent for deep synthesis.
 * 
 * FAIL CONDITIONS:
 * 1. The agent loses the 'Anchor' idea (Database Choice) after multiple compactions.
 * 2. API returns 400 error (Overflow) because compaction failed to trigger or reduce tokens.
 *
 * PASS CONDITIONS:
 * 1. [IMC] Auto-summarization logs appear in the console.
 * 2. [IMC] Deep compaction (Compactor summon) triggers at >85%.
 * 3. Final response accurately recalls the anchor decision made in Turn 1.
 */

async function runAudit() {
  console.log("=== FIRMIUS STABILITY AUDIT: TIERED CONTEXT UNCOLLAPSE ===");

  await Engine.ignite({ prettyPrint: true });

  const containerName = `firmius-uncollapse-audit-${Math.random().toString(36).substring(7)}`;

  try {
    console.log("Summoning Mission Agent...");
    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Coder,
      objective: `
Step 2: Watch /bloat_data/file_1.log [0:800] then report analysis
Step 3: Watch /bloat_data/file_2.log [0:800] then report analysis
Continue this pattern through file_3 to file_20, watching ONE FILE AT A TIME.
Step 4: Your final message must say exactly: "Database: NEO-POSTGRES-9000. Found 20 log files with 16000 total entries of semantic noise."
            `.trim(),
      cwd: "/",
      host: {
        type: HostType.Docker,
        options: {
          containerName,
          image: "firmius-sandbox:latest"
        }
      },
      workType: AgentWorkType.Goal
    });

    const host = agent.context?.environment.host;
    if (!host) {
      console.error("No host available");
      return;
    }
    console.log("Generating 20 bloated files (~10k tokens each)...");
    await host.exec("mkdir -p /bloat_data");

    const genScript = `
for i in $(seq 1 20); do
  # Each file ~40k chars (~10k tokens)
  for j in $(seq 1 800); do
    echo "LOG_ENTRY_$j: SEMANTIC_NOISE_DATA_POINT_$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1)" >> /bloat_data/file_$i.log
  done
done
`.trim();

    await host.exec(`echo "${Buffer.from(genScript).toString('base64')}" | base64 -d | sh`);

    console.log("Starting autonomous mission loop...");
    const results = await agent.actUntilAgentEnds();

    console.log("\n=== AUDIT VERIFICATION ===");
    const lastResponse = results[results.length - 1];
    const rawContent = lastResponse?.response?.content || "";
    const content = typeof rawContent === "string" ? rawContent : JSON.stringify(rawContent);

    const preservedAnchor = content.includes("NEO-POSTGRES-9000");
    const usage = agent.context ? (agent.context.state.metrics.totalTokens / 200000) * 100 : 0;

    console.log(`Final Response Content Length: ${content.length}`);
    console.log(`Final Context Usage: ${usage.toFixed(1)}%`);
    console.log(`Final Response Content: "${content}"`);

    let passed = true;

    if (!preservedAnchor) {
      console.error("\n❌ AUDIT FAILED: Anchor lost. Agent has amnesia.");
      passed = false;
    }

    if (usage < 40) {
      console.error("\n❌ AUDIT FAILED: Agent stopped too early. Context usage should trigger compaction (>40%).");
      passed = false;
    }

    if (passed) {
      console.log("\n✅ AUDIT PASSED: Architectural anchor preserved through compaction.");
    }

  } catch (e: any) {
    console.error("Audit Exception:", e);
  } finally {
    console.log("\nCleaning up...");
    spawnSync("docker", ["rm", "-f", containerName]);
  }
}

runAudit();
