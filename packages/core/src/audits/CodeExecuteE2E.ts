import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType } from "@firmius/shared";
import type { IHost } from "@firmius/shared";
import { DockerHost } from "@firmius/core/hosts";

interface AuditResult {
  passed: boolean;
  tests: Array<{ name: string; passed: boolean; message: string }>;
  durationMs: number;
}

const TASK = `
Your task is to analyze GitHub events and create a summary report:

1. Fetch the latest 30 events from https://api.github.com/events
2. Count occurrences of each event type
3. Identify the top 3 most active repositories
4. Calculate average time between events (if timestamps available)
5. Save a JSON report to /tmp/github_analysis.json with this structure:
{
  "event_counts": { "PushEvent": 15, ... },
  "top_repositories": ["owner/repo1", ...],
  "avg_time_between_events_sec": 12.5,
  "generated_at": "2024-01-15T10:30:00Z"
}

You must use code_execute for all data processing. The session maintains variables between calls.
`;

export async function runCodeExecuteE2EAudit(): Promise<AuditResult> {
  const tests: AuditResult["tests"] = [];
  const startTime = Date.now();

  // Create host
  const host: IHost = new DockerHost({
    image: "firmius-sandbox:latest",
    containerName: "omgbro",
  });
  await host.init();
  const cwd = host.defaultCwd.toString();

  try {
    await Engine.ignite({ prettyPrint: true });

    const agent = await Engine.agentFactory.summon({
      purpose: BuiltinPurposes.Coder,
      objective: TASK,
      cwd,
      host,
      workType: AgentWorkType.Goal,
      generationOptions: { providerId: "opencode", modelId: "big-pickle" },
      disableCompaction: true,
      constraints: { allowOutsideCwd: true }
    });

    const result = await agent.actUntilAgentEnds();

    const durationMs = Date.now() - startTime;

    // Check if report file was created
    const reportExists = await host.exists("/tmp/github_analysis.json");

    tests.push({
      name: "Report file created",
      passed: reportExists,
      message: reportExists ? "Report exists at /tmp/github_analysis.json" : "Report file not found"
    });

    if (reportExists) {
      const content = await host.readFile("/tmp/github_analysis.json");
      let json: any = null;
      let jsonValid = false;

      try {
        json = JSON.parse(content);
        jsonValid = true;
        tests.push({
          name: "Valid JSON",
          passed: true,
          message: "Report is valid JSON"
        });
      } catch (err) {
        jsonValid = false;
        tests.push({
          name: "Valid JSON",
          passed: false,
          message: `JSON parse error: ${err instanceof Error ? err.message : String(err)}`
        });
      }

      if (!jsonValid) {
        // Can't continue with JSON checks
      } else {
        tests.push({
          name: "event_counts present",
          passed: !!json.event_counts && typeof json.event_counts === 'object',
          message: json.event_counts ? `Found ${Object.keys(json.event_counts).length} event types` : "Missing event_counts"
        });

        tests.push({
          name: "top_repositories present",
          passed: Array.isArray(json.top_repositories) && json.top_repositories.length >= 3,
          message: json.top_repositories ? `Found ${json.top_repositories.length} top repos` : "Missing or too few top_repositories"
        });

        tests.push({
          name: "avg_time_between_events_sec present",
          passed: typeof json.avg_time_between_events_sec === 'number',
          message: json.avg_time_between_events_sec ? `Avg time: ${json.avg_time_between_events_sec}s` : "Missing or invalid avg_time"
        });

        tests.push({
          name: "generated_at timestamp present",
          passed: !!json.generated_at,
          message: json.generated_at ? `Generated at: ${json.generated_at}` : "Missing timestamp"
        });
      }
    }

    // Check efficiency - should complete in reasonable number of turns
    const turnCount = result.length;
    tests.push({
      name: "Efficiency (turns)",
      passed: turnCount <= 10,
      message: `Completed in ${turnCount} turns`
    });

    const allPassed = tests.every(t => t.passed);

    return {
      passed: allPassed,
      tests,
      durationMs
    };

  } catch (e: any) {
    tests.push({
      name: "Audit execution",
      passed: false,
      message: `Fatal error: ${e.message}`
    });
    return {
      passed: false,
      tests,
      durationMs: Date.now() - startTime
    };
  } finally {
    await host.destroy();
  }
}

// Run if called directly
if (import.meta.main) {
  runCodeExecuteE2EAudit().then(result => {
    console.log("\n" + "=".repeat(60));
    console.log("CODE EXECUTE E2E AUDIT");
    console.log("=".repeat(60));
    console.log(`\nDuration: ${(result.durationMs / 1000).toFixed(2)}s`);
    console.log(`Result: ${result.passed ? '✅ PASSED' : '❌ FAILED'}`);

    console.log("\nTests:");
    for (const test of result.tests) {
      const status = test.passed ? "✓" : "✗";
      console.log(`  ${status} ${test.name}: ${test.message}`);
    }

    const failed = result.tests.filter(t => !t.passed).length;
    process.exit(failed > 0 ? 1 : 0);
  });
}
