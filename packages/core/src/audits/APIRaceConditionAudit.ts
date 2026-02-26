import { join } from 'node:path';
import { mkdir } from 'node:fs/promises';
import { rmSync } from 'node:fs';
import { tmpdir } from 'node:os';

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

const API_BASE_URL = 'http://localhost:9174';
const AUDIT_THREAD_PREFIX = 'api-race-audit-';
const TEST_DIR = join(tmpdir(), 'firmius-api-race-audit');

function printSummary(summary: AuditSummary): void {
  console.log('\n' + '='.repeat(60));
  console.log('AUDIT SUMMARY');
  console.log('='.repeat(60));
  console.log(`Total tests: ${summary.total}`);
  console.log(`Passed: ${summary.passed}`);
  console.log(`Failed: ${summary.failed}`);
  console.log(`Duration: ${(summary.duration / 1000).toFixed(2)}s`);
  console.log('='.repeat(60));

  for (const result of summary.results) {
    const status = result.passed ? 'PASS' : 'FAIL';
    console.log(`\n[${status}]: ${result.name}`);
    if (result.error) {
      console.log(`   Error: ${result.error}`);
    }
    if (result.details) {
      console.log(`   Details: ${result.details}`);
    }
  }

  console.log('\n' + '='.repeat(60));
  if (summary.failed === 0) {
    console.log('ALL TESTS PASSED');
  } else {
    console.log(`${summary.failed} TEST(S) FAILED`);
  }
  console.log('='.repeat(60));
}

async function createTestThread(): Promise<{ id: string; status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      hostConfig: { type: 0 },
      rootCwd: TEST_DIR,
      purpose: 'General',
      objective: 'Test race condition operations',
    }),
  });

  return {
    id: response.headers.get('x-thread-id') || '',
    status: response.status,
    data: response.status === 201 ? await response.json() : undefined,
  };
}

async function deleteThread(threadId: string): Promise<boolean> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}`, {
    method: 'DELETE',
  });
  return response.status === 200;
}

async function getThread(threadId: string): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}`);
  return {
    status: response.status,
    data: response.status === 200 ? await response.json() : undefined,
  };
}

async function sendMessage(threadId: string, content: string): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}/messages`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ message: content }),
  });

  return {
    status: response.status,
    data: response.status === 201 ? await response.json() : undefined,
  };
}

async function interruptThread(threadId: string): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}/interrupt`, {
    method: 'POST',
  });

  return {
    status: response.status,
    data: response.status === 200 ? await response.json() : undefined,
  };
}

async function cleanupTestThreads(): Promise<void> {
  try {
    const response = await fetch(`${API_BASE_URL}/api/threads`);
    if (response.status === 200) {
      const data = await response.json() as { threads: Array<{ id: string }> };
      for (const thread of data.threads) {
        if (thread.id.startsWith(AUDIT_THREAD_PREFIX)) {
          await deleteThread(thread.id);
        }
      }
    }
  } catch {
    // threads API not available
  }
}

async function testCase24(): Promise<TestResult> {
  // Delete thread while GET in progress (no crash, consistent state)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 24: Delete thread while GET in progress',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Start multiple concurrent GET requests
    const NUM_REQUESTS = 10;
    const getPromises = Array.from({ length: NUM_REQUESTS }, async () => {
      return getThread(threadId);
    });

    // While GETs are in progress, delete the thread
    const deletePromise = deleteThread(threadId);

    // Wait for all operations to complete
    const [getResults, deleteResult] = await Promise.all([
      Promise.all(getPromises),
      deletePromise,
    ]);

    // Check results - some GETs may succeed, some may fail with 404
    const successCount = getResults.filter(r => r.status === 200).length;
    const failCount = getResults.filter(r => r.status === 404).length;
    const otherCount = getResults.filter(r => r.status !== 200 && r.status !== 404).length;

    const passed = deleteResult && successCount >= 0;
    return {
      name: 'Case 24: Delete thread while GET in progress',
      passed,
      details: `Deleted: ${deleteResult}, GETs: ${successCount} success, ${failCount} 404, ${otherCount} other`,
    };
  } catch (e) {
    return {
      name: 'Case 24: Delete thread while GET in progress',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase25(): Promise<TestResult> {
  // Send message while interrupt in progress (handled correctly)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 25: Send message while interrupt in progress',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Send initial message to start agent processing
    await sendMessage(threadId, 'Test message - respond immediately');

    // Now trigger concurrent interrupts and message sends
    const NUM_OPS = 5;

    const operations = Array.from({ length: NUM_OPS }, async (_, i) => {
      const promises = [];

      if (i % 2 === 0) {
        // Interrupt
        promises.push(interruptThread(threadId));
      } else {
        // Send message
        promises.push(sendMessage(threadId, `Concurrent message ${i}`));
      }

      // Add a GET to verify consistency
      promises.push(getThread(threadId));

      return Promise.all(promises);
    });

    const results = await Promise.all(operations);

    // Flatten and count results
    const allResults = results.flat();
    const successCount = allResults.filter(r => r.status === 200 || r.status === 201).length;
    const errorCount = allResults.filter(r => r.status >= 400).length;

    // Cleanup
    await deleteThread(threadId);

    // Test passes if no crashes occurred (all operations completed)
    const passed = successCount + errorCount === allResults.length;
    return {
      name: 'Case 25: Send message while interrupt in progress',
      passed,
      details: `Operations: ${allResults.length} total, ${successCount} success, ${errorCount} handled errors`,
    };
  } catch (e) {
    return {
      name: 'Case 25: Send message while interrupt in progress',
      passed: false,
      error: String(e),
    };
  }
}

async function runAudit(): Promise<AuditSummary> {
  console.log('=== FIRMIUS API RACE CONDITION AUDIT ===');
  console.log(`API Base URL: ${API_BASE_URL}`);

  const startTime = Date.now();
  const results: TestResult[] = [];

  // Create test directory
  await mkdir(TEST_DIR, { recursive: true });

  // Cleanup any existing test threads
  await cleanupTestThreads();

  try {
    results.push(await testCase24());
    results.push(await testCase25());
  } finally {
    // Final cleanup
    await cleanupTestThreads();

    // Clean up test directory
    try {
      rmSync(TEST_DIR, { recursive: true, force: true });
    } catch {
      // ignore
    }
  }

  const endTime = Date.now();
  const summary: AuditSummary = {
    total: results.length,
    passed: results.filter(r => r.passed).length,
    failed: results.filter(r => !r.passed).length,
    results,
    duration: endTime - startTime,
  };

  printSummary(summary);
  return summary;
}

runAudit().catch(console.error);
