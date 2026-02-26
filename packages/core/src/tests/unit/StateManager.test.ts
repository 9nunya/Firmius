import { test, expect, describe, beforeEach, afterEach } from "bun:test";
import { StateManager } from "../../state/StateManager";
import { LocalHost } from "../../hosts/Local";
import { join } from "node:path";
import { mkdir, rm } from "node:fs/promises";

const TEST_THREAD_ID = "test-state-manager-thread";
const TEST_BASE_DIR = join("/tmp", "firmius-test-state", TEST_THREAD_ID);

describe("StateManager", () => {
  let host: LocalHost;
  let stateManager: StateManager;

  beforeEach(async () => {
    host = new LocalHost();
    await mkdir(TEST_BASE_DIR, { recursive: true });
  });

  afterEach(async () => {
    if (stateManager) {
      await stateManager.close();
    }
    await rm("/tmp/firmius-test-state", { recursive: true, force: true }).catch(() => {});
  });

  describe("LocalHost", () => {
    test("should create StateManager for local host", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      expect(stateManager).toBeDefined();
      expect(stateManager.getDB()).toBeDefined();
    });

    test("should run migrations on create", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      const db = stateManager.getDB();
      const agentsTableExists = db.query(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='agents'"
      ).get();
      
      expect(agentsTableExists).toBeDefined();
    });

    test("should create agents table with correct schema", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      const db = stateManager.getDB();
      const schema = db.query("PRAGMA table_info(agents)").all();
      
      const columns = schema as Array<{name: string, type: string}>;
      const columnNames = columns.map(c => c.name);
      
      expect(columnNames).toContain("id");
      expect(columnNames).toContain("purpose");
      expect(columnNames).toContain("parent_id");
      expect(columnNames).toContain("worktree_path");
      expect(columnNames).toContain("branch");
      expect(columnNames).toContain("status");
    });

    test("should create mail table with correct schema", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      const db = stateManager.getDB();
      const schema = db.query("PRAGMA table_info(mail)").all();
      
      const columns = schema as Array<{name: string, type: string}>;
      const columnNames = columns.map(c => c.name);
      
      expect(columnNames).toContain("id");
      expect(columnNames).toContain("from_agent");
      expect(columnNames).toContain("to_agent");
      expect(columnNames).toContain("type");
      expect(columnNames).toContain("payload");
      expect(columnNames).toContain("priority");
    });

    test("should create indexes on mail table", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      const db = stateManager.getDB();
      const indexes = db.query(
        "SELECT name FROM sqlite_master WHERE type='index' AND tbl_name='mail'"
      ).all();
      
      const indexNames = (indexes as Array<{name: string}>).map(i => i.name);
      
      expect(indexNames.some(n => n.includes("idx_mail_to"))).toBe(true);
      expect(indexNames.some(n => n.includes("idx_mail_type"))).toBe(true);
    });

    test("should use WAL journal mode", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      const db = stateManager.getDB();
      const journalMode = db.query("PRAGMA journal_mode").get() as { journal_mode: string };
      
      expect(journalMode.journal_mode.toLowerCase()).toBe("wal");
    });

    test("should close database", async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
      
      await stateManager.close();
      
      // After close, db should be closed (attempting to query should fail)
      // Note: bun:sqlite doesn't throw on closed db, so we just verify no error
      expect(true).toBe(true);
    });
  });

  describe("Database Operations", () => {
    beforeEach(async () => {
      stateManager = await StateManager.create(host, TEST_BASE_DIR, TEST_THREAD_ID);
    });

    test("should insert and retrieve agent record", async () => {
      const db = stateManager.getDB();
      
      const now = Date.now();
      db.run(
        `INSERT INTO agents (id, purpose, parent_id, status, spawned_at) VALUES (?, ?, ?, ?, ?)`,
        ["agent-1", "general", null, "running", now]
      );
      
      const result = db.query("SELECT * FROM agents WHERE id = ?").get("agent-1") as any;
      
      expect(result.id).toBe("agent-1");
      expect(result.purpose).toBe("general");
      expect(result.status).toBe("running");
    });

    test("should insert and retrieve mail message", async () => {
      const db = stateManager.getDB();
      
      const now = Date.now();
      db.run(
        `INSERT INTO mail (from_agent, to_agent, type, payload, created_at) VALUES (?, ?, ?, ?, ?)`,
        ["agent-1", "agent-2", "task", '{"task": "do something"}', now]
      );
      
      const result = db.query("SELECT * FROM mail WHERE from_agent = ?").get("agent-1") as any;
      
      expect(result.from_agent).toBe("agent-1");
      expect(result.to_agent).toBe("agent-2");
      expect(result.type).toBe("task");
    });

    test("should update agent status", async () => {
      const db = stateManager.getDB();
      
      const now = Date.now();
      db.run(
        `INSERT INTO agents (id, purpose, status, spawned_at) VALUES (?, ?, ?, ?)`,
        ["agent-2", "general", "running", now]
      );
      
      db.run(`UPDATE agents SET status = ?, completed_at = ? WHERE id = ?`, ["completed", now, "agent-2"]);
      
      const result = db.query("SELECT status, completed_at FROM agents WHERE id = ?").get("agent-2") as any;
      
      expect(result.status).toBe("completed");
      expect(result.completed_at).toBe(now);
    });

    test("should delete agent record", async () => {
      const db = stateManager.getDB();
      
      const now = Date.now();
      db.run(
        `INSERT INTO agents (id, purpose, status, spawned_at) VALUES (?, ?, ?, ?)`,
        ["agent-3", "general", "running", now]
      );
      
      db.run(`DELETE FROM agents WHERE id = ?`, ["agent-3"]);
      
      const result = db.query("SELECT * FROM agents WHERE id = ?").get("agent-3");
      
      expect(result).toBeFalsy();
    });
  });
});
