import { Engine } from "@firmius/core";
import { BuiltinPurposes } from "@firmius/shared";
import { HostType } from "@firmius/shared";
import { spawnSync } from "node:child_process";
import { Agent } from "@firmius/core";

/**
 * REASONING:
 * Tests recursive lifecycle integrity. When an agent is terminated, all its
 * assets (processes, children, file watches) should be reaped to prevent 
 * resource exhaustion, especially on shared Docker or SSH hosts.
 *
 * FAIL CONDITIONS:
 * 1. ProcessManager still tracks PIDs of a terminated agent's processes.
 * 2. Background processes are still running on the Host after agent termination.
 * 3. Child agents remain active in the AgentFactory after parent termination.
 *
 * PASS CONDITIONS:
 * 1. Terminating a Root agent triggers a recursive cleanup of the entire subtree.
 * 2. All spawned PIDs associated with the subtree are killed.
 * 3. All associated Host resources are released.
 */

async function runAudit() {
    console.log("=== FIRMIUS STABILITY AUDIT: ZOMBIE APOCALYPSE ===");
    
    await Engine.ignite();

    const containerName = `firmius-zombie-audit-${Math.random().toString(36).substring(7)}`;

    try {
        // 1. Create Root Agent
        console.log("Summoning Root Agent...");
        const rootAgent = await Engine.agentFactory.summon({
            purpose: BuiltinPurposes.Coder,
            objective: "Root Agent",
            cwd: "/",
            host: {
                type: HostType.Docker,
                options: {
                    containerName,
                    image: "firmius-sandbox:latest"
                }
            }
        });

        // 2. Root Agent spawns a process
        console.log("Root Agent spawning process...");
        const rootRes = await (rootAgent as Agent).executeTool("process_control", { operation: "spawn", command: "while true; do echo root >> /root.log; sleep 1; done" }, {});
        console.log(`Root Process PID: ${rootRes.output?.pid}`);

        // 3. Root Agent summons Child
        console.log("Root Agent summoning Child Agent...");
        const childAgent = await Engine.agentFactory.summon({
            purpose: BuiltinPurposes.Coder,
            objective: "Child Agent",
            cwd: "/",
            host: "inherit",
            parentId: rootAgent.id
        });

        // 4. Child Agent spawns a process
        console.log("Child Agent spawning process...");
        const childRes = await (childAgent as Agent).executeTool("process_control", { operation: "spawn", command: "while true; do echo child >> /child.log; sleep 1; done" }, {});
        console.log(`Child Process PID: ${childRes.output?.pid}`);

        // 5. Child Agent summons Grandchild
        console.log("Child Agent summoning Grandchild Agent...");
        const grandchildAgent = await Engine.agentFactory.summon({
            purpose: BuiltinPurposes.Coder,
            objective: "Grandchild Agent",
            cwd: "/",
            host: "inherit",
            parentId: childAgent.id
        });

        // 6. Grandchild spawns a process
        console.log("Grandchild Agent spawning process...");
        const grandchildRes = await (grandchildAgent as Agent).executeTool("process_control", { operation: "spawn", command: "while true; do echo grandchild >> /grandchild.log; sleep 1; done" }, {});
        console.log(`Grandchild Process PID: ${grandchildRes.output?.pid}`);

        console.log("\n--- Hierarchy established ---");
        console.log(`Total Agents: ${Engine.agentFactory.agents.size}`);
        console.log(`Total Processes: ${Engine.processManager.list().length}`);

        // 7. THE BREAK: Terminate the Root Agent
        console.log(`\nTerminating Root Agent [${rootAgent.id}]...`);
        await Engine.agentFactory.terminate(rootAgent.id);

        console.log("\n--- Audit Verification ---");

        // Verification 1: Agent Registry
        const agentsLeft = Engine.agentFactory.agents.size;
        console.log(`Agents remaining in registry: ${agentsLeft}`);
        
        // Verification 2: Process Manager
        const procsLeft = Engine.processManager.list().length;
        console.log(`Processes remaining in manager: ${procsLeft}`);

        // Verification 3: Host OS check (Docker ps)
        console.log("Checking Docker container for leaked processes...");
        const psRes = spawnSync("docker", ["exec", containerName, "ps", "aux"]);
        const psOutput = psRes.stdout.toString();
        
        const hasRoot = psOutput.includes("root.log");
        const hasChild = psOutput.includes("child.log");
        const hasGrandchild = psOutput.includes("grandchild.log");

        console.log("Ghost processes found:");
        console.log(`- Root: ${hasRoot}`);
        console.log(`- Child: ${hasChild}`);
        console.log(`- Grandchild: ${hasGrandchild}`);

        if (agentsLeft === 0 && procsLeft === 0 && !hasRoot && !hasChild && !hasGrandchild) {
            console.log("\n✅ AUDIT PASSED: Recursive cleanup successful.");
        } else {
            console.error("\n❌ AUDIT FAILED: System is leaking resources.");
            if (agentsLeft > 0) console.error(`- ${agentsLeft} agents leaked in registry`);
            if (procsLeft > 0) console.error(`- ${procsLeft} process handles leaked in manager`);
            if (hasRoot || hasChild || hasGrandchild) console.error("- Ghost processes remain in host");
        }

    } catch (e) {
        console.error("Audit crashed:", e);
    } finally {
        console.log("\nCleaning up container...");
        // Use direct docker command to ensure cleanup if factory failed
        spawnSync("docker", ["rm", "-f", containerName]);
    }
}

runAudit();
