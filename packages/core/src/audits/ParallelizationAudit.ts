import { Engine } from "@firmius/core";
import { BuiltinPurposes, AgentWorkType, type IAgent } from "@firmius/shared";
import { HostType } from "@firmius/shared";
import { Agent } from "@firmius/core";
import { ToolScope } from "@firmius/shared";
import { spawnSync } from "node:child_process";

interface AuditEvent {
  type: string;
  agentId: string;
  timestamp: number;
  parentId?: string;
  purpose?: string;
  [key: string]: any;
}

async function runAudit() {
  console.log("=== FIRMIUS PARALLELIZATION & ORCHESTRATION AUDIT ===");

  const spawnEvents: AuditEvent[] = [];
  const terminationEvents: AuditEvent[] = [];
  const toolCallEvents: AuditEvent[] = [];

  const onAgentSpawn = (e: AuditEvent) => {
    spawnEvents.push({ ...e, timestamp: Date.now() });
  };
  const onAgentTerminated = (e: AuditEvent) => {
    terminationEvents.push({ ...e, timestamp: Date.now() });
  };
  const onToolCall = (e: AuditEvent) => {
    toolCallEvents.push(e);
  };

  Engine.eventEmitter.on('agent_spawned', onAgentSpawn);
  Engine.eventEmitter.on('agent_terminated', onAgentTerminated);
  Engine.eventEmitter.on('tool_call_start', onToolCall);

  await Engine.ignite({ prettyPrint: true });

  const containerName = `parallel-audit-${Math.random().toString(36).substring(2, 10)}`;

   try {
     console.log(`Creating Docker container: ${containerName}`);
     const leadAgent = await Engine.agentFactory.summon({
        purpose: BuiltinPurposes.General,
       objective: getLeadObjective(),
       cwd: "/work",
       host: {
         type: HostType.Docker,
         options: {
           containerName,
           image: "firmius-sandbox:latest"
         }
       },
       workType: AgentWorkType.Conversational,
       generationOptions: { providerId: "zai", modelId: "glm-4.7" }
     });

      // Ensure lead agent has necessary scopes for delegation, process, and filesystem write
      const lead = leadAgent as Agent;
      const neededScopes = [ToolScope.Delegation, ToolScope.Process, ToolScope.FilesystemWrite];
      for (const scope of neededScopes) {
        if (!lead.context.environment.permissions.scopes.includes(scope)) {
          lead.context.environment.permissions.scopes.push(scope);
        }
      }

     console.log(`Lead agent [${leadAgent.id}] created. Starting orchestration test...`);
    const startTime = Date.now();
    const results = await leadAgent.actUntilAgentEnds();
    const endTime = Date.now();
    const totalElapsed = endTime - startTime;

    const lastResponse = results[results.length - 1];
    const rawFinalMessage = lastResponse?.response?.content || "";
    const finalMessage = typeof rawFinalMessage === "string" ? rawFinalMessage : JSON.stringify(rawFinalMessage);

    console.log(`\n=== AUDIT RESULTS ===`);
    console.log(`Total runtime: ${(totalElapsed / 1000).toFixed(2)}s`);
    console.log(`Final message:\n${finalMessage}`);

    const passed = verifyAudit(spawnEvents, leadAgent as IAgent, finalMessage);
    console.log(passed ? "\n✅ AUDIT PASSED" : "\n❌ AUDIT FAILED");

  } catch (e: any) {
    console.error("Audit Exception:", e);
  } finally {
    Engine.eventEmitter.off('agent_spawned', onAgentSpawn);
    Engine.eventEmitter.off('agent_terminated', onAgentTerminated);
    Engine.eventEmitter.off('tool_call_start', onToolCall);

    try {
      // Ignore if container doesn't exist
      spawnSync("docker", ["rm", "-f", containerName], { stdio: "ignore" });
      console.log(`Container ${containerName} removed.`);
    } catch (err) {
      // ignore cleanup errors
    }
  }
}

function getLeadObjective(): string {
  return `
You are an orchestrator agent testing parallelization and delegation restrictions. Follow these steps EXACTLY and in order:

**Working Directory**: /work

**Step 1: Spawn 3 parallel Researcher subagents**
- Use agent_spawn with these exact parameters (all host="inherit", cwd="/work"):
  - researcher1: objective: "Use fetch_url to call https://api.github.com/repos/expressjs/express. Extract stargazers_count, forks_count, open_issues_count. Return one line: 'Express: stars X, forks Y, issues Z'."
  - researcher2: objective: "Use fetch_url to call https://api.github.com/repos/facebook/react. Extract stargazers_count, forks_count, open_issues_count. Return one line: 'React: stars X, forks Y, issues Z'."
  - researcher3: objective: "Use fetch_url to call https://api.github.com/repos/vuejs/vue. Extract stargazers_count, forks_count, open_issues_count. Return one line: 'Vue: stars X, forks Y, issues Z'."
- Each must have a unique agentId (researcher1, researcher2, researcher3).
- This should happen in a single turn (parallel spawns).

**Step 2: Wait for each researcher**
- agent_await({ agentId: "researcher1", timeout: 300 })
- agent_await({ agentId: "researcher2", timeout: 300 })
- agent_await({ agentId: "researcher3", timeout: 300 })
- Capture each result; do not proceed until all three have completed.

**Step 3: Verify results**
- All three must include numeric values. If any failed, note it but continue.

**Step 4: Spawn a Coder subagent**
- agent_spawn with agentId "coder1", purpose "Coder", cwd="/work", host="inherit".
- objective: "Using absolute paths under /work/coder1_workspace:
  1) git clone --depth 1 https://github.com/expressjs/express.git /work/coder1_workspace/express
  2) cat /work/coder1_workspace/express/package.json, extract 'description' and 'dependencies' object. Report the description and the top 3 dependency names.
  3) find /work/coder1_workspace/express/lib -name '*.js' | wc -l to count .js files
  4) Provide a concise summary (under 200 words) including description, top 3 dependencies, and .js count."

**Step 5: Wait for coder1**
- agent_await({ agentId: "coder1", timeout: 300 })

**Step 6: Spawn a Coder for a long task**
- agent_spawn with agentId "long1", purpose "Coder", cwd="/work", host="inherit".
- objective: "Using absolute paths under /work/long1_workspace:
  1) git clone --depth 1 https://github.com/microsoft/vscode.git /work/long1_workspace/vscode (this may take a minute or more)
  2) find /work/long1_workspace/vscode -name '*.ts' | wc -l to count .ts files
  3) cat /work/long1_workspace/vscode/README.md and summarize what VSCode is in 1-2 sentences
  4) Return the .ts count and summary."
- Use host="inherit"

**Step 7: Wait for long1 with extended timeout**
- agent_await({ agentId: "long1", timeout: 900 })

**Step 8: Test Goal agent spawn restriction**
- Spawn a Goal-type agent: agent_spawn with agentId "goal_tester", purpose "General", cwd="/work", host="inherit". Subagents default to Goal worktype, so goal_tester will be Goal.
- Provide objective: "You are a Goal agent with Delegation scope. Test whether you can spawn a subagent: 1) Use agent_spawn to create helper with agentId='helper' and objective='Return the string done.' 2) Immediately agent_await({ agentId: 'helper', timeout: 30 }) 3) Report: Did spawn succeed? Include exact message if error. Expected: Spawn should be BLOCKED because Goal agents cannot spawn subagents."
- Spawn with agent_spawn.

**Step 9: Wait for goal_tester**
- agent_await({ agentId: "goal_tester", timeout: 300 })

**Step 10: Compose final report**
- Include:
  * Results from the three researchers (Express/React/Vue stats)
  * coder1 summary (Express description, deps, .js count)
  * long1 result (.ts count and VSCode summary)
  * Whether goal_tester's spawn attempt was blocked
- EXACTLY include these concluding lines:
    "PARALLELIZATION TEST RESULT: PASS" (if all subagents succeeded AND goal_tester reported blocked)
    "GOAL AGENT SPAWN TEST: BLOCKED"
  If any condition fails, include:
    "PARALLELIZATION TEST RESULT: FAIL" and explain why
    "GOAL AGENT SPAWN TEST: UNBLOCKED" if goal_tester succeeded in spawning

**Step 11**: Complete your mission with complete_task(reason="respond").

Make sure to follow this exact sequence. Do not deviate.
`.trim();
}

function verifyAudit(
  spawnEvents: AuditEvent[],
  leadAgent: IAgent,
  finalMessage: string
): boolean {
  const leadId = leadAgent.id;

  // 1. Count subagents spawned by lead
  const leadSpawns = spawnEvents.filter(e => e.parentId === leadId);
  const subagentIds = new Set(leadSpawns.map(e => e.agentId));
  console.log(`Spawned subagents (${subagentIds.size}): ${Array.from(subagentIds).join(', ')}`);

  // Expect at least 6 subagents (3x Researcher, 1 Coder, 1 Coder long, 1 General goal_tester)
  if (subagentIds.size < 6) {
    console.error(`Expected at least 6 subagents, got ${subagentIds.size}`);
    return false;
  }

  // 2. Parallel spawn window: timestamps of first three Researcher spawns
  const researcherSpawns = leadSpawns.filter(e => e.purpose === 'Researcher');
  if (researcherSpawns.length >= 3) {
    const sorted = researcherSpawns.sort((a, b) => (a.timestamp || 0) - (b.timestamp || 0));
    if (sorted.length >= 3) {
      const ts1 = sorted[0]?.timestamp ?? 0;
      const ts3 = sorted[2]?.timestamp ?? 0;
      const window = ts3 - ts1;
      console.log(`Parallel spawn window for 3 researchers: ${window}ms`);
      if (window > 10000) {
        console.error(`Parallel spawns took too long (${window}ms)`);
        return false;
      }
    }
  }

  // 3. Check no unauthorized sub-spawns: any spawn with a parentId that is not leadId is unauthorized
  const unauthorizedSpawns = spawnEvents.filter(e => e.parentId !== undefined && e.parentId !== leadId);
  if (unauthorizedSpawns.length > 0) {
    console.error(`Unauthorized spawn events (from non-lead parents): ${unauthorizedSpawns.length}`);
    return false;
  }

  // 4. Final message markers
  const hasPassMarker = finalMessage.includes("PARALLELIZATION TEST RESULT: PASS");
  const hasBlockedMarker = finalMessage.includes("GOAL AGENT SPAWN TEST: BLOCKED");
  if (!hasPassMarker) {
    console.error("Final message missing PARALLELIZATION TEST RESULT: PASS");
  }
  if (!hasBlockedMarker) {
    console.error("Final message missing GOAL AGENT SPAWN TEST: BLOCKED");
  }

  return hasPassMarker && hasBlockedMarker;
}

runAudit();
