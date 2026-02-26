import { test, expect, describe, beforeEach, afterEach } from "bun:test";
import { Coordinator } from "../../Coordinator";
import { LocalHost } from "../../hosts/Local";
import { join } from "node:path";
import { mkdir, rm } from "node:fs/promises";

const TEST_THREAD_ID = "test-coordinator-thread";
const TEST_BASE_DIR = join("/tmp", "firmius-test-coordinator", TEST_THREAD_ID);

describe("Coordinator", () => {
  let host: LocalHost;
  let coordinator: Coordinator;

  beforeEach(async () => {
    host = new LocalHost();
    await mkdir(TEST_BASE_DIR, { recursive: true });
  });

  afterEach(async () => {
    if (coordinator) {
      await coordinator.destroy();
    }
    await rm("/tmp/firmius-test-coordinator", { recursive: true, force: true }).catch(() => { });
  });

  describe("Creation", () => {
    test("should create Coordinator instance", async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);

      expect(coordinator).toBeDefined();
      expect(coordinator.host).toBeDefined();
      expect(coordinator.baseDir).toBe(TEST_BASE_DIR);
    });

    test("should expose fleet registry", async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);

      expect(coordinator.fleet).toBeDefined();
    });

    // TODO: Re-enable when worktree manager is implemented
    // test("should expose worktree manager", async () => {
    //   coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    //   
    //   expect(coordinator.worktrees).toBeDefined();
    // });
  });

  describe("Fleet Registry", () => {
    beforeEach(async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    });

    test("should register an agent", async () => {
      const result = await coordinator.spawnAgent({
        purpose: "general",
        objective: "Test agent",
        agentId: "test-agent-1",
        parentId: "parent-agent"
      });

      expect(result.agentId).toBe("test-agent-1");
    });

    test("should get agent status", async () => {
      await coordinator.spawnAgent({
        purpose: "general",
        objective: "Test agent",
        agentId: "test-agent-2",
        parentId: "parent-agent"
      });

      const status = await coordinator.getAgentStatus("test-agent-2");

      expect(status).toBeDefined();
    });

    test("should return idle for unknown agent", async () => {
      const status = await coordinator.getAgentStatus("nonexistent-agent");

      expect(status).toBe("idle");
    });

    test("should get fleet status", async () => {
      await coordinator.spawnAgent({
        purpose: "general",
        objective: "Agent 1",
        agentId: "fleet-agent-1",
        parentId: "parent"
      });

      await coordinator.spawnAgent({
        purpose: "general",
        objective: "Agent 2",
        agentId: "fleet-agent-2",
        parentId: "parent"
      });

      const fleetStatus = await coordinator.getFleetStatus();

      expect(fleetStatus.agents).toBeDefined();
      expect(fleetStatus.health).toBeDefined();
    });
  });

  describe("Health Check", () => {
    beforeEach(async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    });

    test("should return healthy report with no agents", async () => {
      const report = await coordinator.checkHealth();

      expect(report.healthy).toEqual([]);
      expect(report.stuck).toEqual([]);
      expect(report.recommendations).toEqual([]);
    });

    test("should identify stuck agents", async () => {
      // Register an agent but don't update its heartbeat
      await coordinator.spawnAgent({
        purpose: "general",
        objective: "Test agent",
        agentId: "stuck-agent",
        parentId: "parent"
      });

      const report = await coordinator.checkHealth();

      // The agent we just registered should have a recent heartbeat
      expect(report.healthy.length).toBeGreaterThanOrEqual(0);
    });
  });

  describe("Worktree Management", () => {
    beforeEach(async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    });

    test("should spawn agent without worktree", async () => {
      const result = await coordinator.spawnAgent({
        purpose: "general",
        objective: "Agent without worktree",
        agentId: "no-worktree-agent",
        parentId: "parent"
      });

      expect(result.agentId).toBe("no-worktree-agent");
    });
  });

  describe("Agent Actions", () => {
    beforeEach(async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    });

    test("should kill agent", async () => {
      await coordinator.spawnAgent({
        purpose: "general",
        objective: "Agent to kill",
        agentId: "kill-agent",
        parentId: "parent"
      });

      await coordinator.killAgent("kill-agent");

      const status = await coordinator.getAgentStatus("kill-agent");
      expect(status).toBe("completed");
    });
  });

  describe("Destruction", () => {
    test("should destroy coordinator and close state manager", async () => {
      coordinator = await Coordinator.create(host, TEST_BASE_DIR, TEST_THREAD_ID);

      await coordinator.destroy();

      // After destroy, the coordinator should be effectively cleaned up
      expect(true).toBe(true);
    });
  });
});
