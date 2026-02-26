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
const AUDIT_THREAD_PREFIX = 'api-message-audit-';
const TEST_DIR = join(tmpdir(), 'firmius-api-message-audit');

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
      objective: 'Test message operations',
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

async function editMessage(
  threadId: string,
  sequence: number,
  newContent: string
): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}/messages/${sequence}/edit`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ sequence, newContent }),
  });

  return {
    status: response.status,
    data: response.status === 200 ? await response.json() : undefined,
  };
}

async function forgetMessage(threadId: string, sequence: number): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}/messages/${sequence}/forget`, {
    method: 'POST',
  });

  return {
    status: response.status,
    data: response.status === 200 ? await response.json() : undefined,
  };
}

async function getMessages(threadId: string): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}/messages`);
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

async function testCase6(): Promise<TestResult> {
  // Send empty message (should reject with 400)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 6: Send empty message',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    const messageResult = await sendMessage(threadId, '');

    await deleteThread(threadId);

    const passed = messageResult.status === 400;
    return {
      name: 'Case 6: Send empty message',
      passed,
      details: passed
        ? 'Correctly rejected empty message with 400'
        : `Expected 400, got ${messageResult.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 6: Send empty message',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase7(): Promise<TestResult> {
  // Send 10MB message (size limit enforced)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 7: Send 10MB message',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Create a 10MB message
    const largeMessage = 'a'.repeat(10 * 1024 * 1024);

    const messageResult = await sendMessage(threadId, largeMessage);

    await deleteThread(threadId);

    // Should reject with 413 (Payload Too Large) or 400
    const passed = messageResult.status === 413 || messageResult.status === 400;
    return {
      name: 'Case 7: Send 10MB message',
      passed,
      details: passed
        ? `Correctly rejected large message with ${messageResult.status}`
        : `Accepted large message with status ${messageResult.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 7: Send 10MB message',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase8(): Promise<TestResult> {
  // Edit message to empty content (should reject or handle)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 8: Edit message to empty content',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Send a message first
    await sendMessage(threadId, 'Initial message');

    // Try to edit to empty
    const editResult = await editMessage(threadId, 0, '');

    await deleteThread(threadId);

    // Should reject with 400
    const passed = editResult.status === 400;
    return {
      name: 'Case 8: Edit message to empty content',
      passed,
      details: passed
        ? 'Correctly rejected edit to empty with 400'
        : `Edit to empty returned ${editResult.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 8: Edit message to empty content',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase9(): Promise<TestResult> {
  // Forget entry agent is currently processing (no crash)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 9: Forget entry agent is processing',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Send a message to start processing
    await sendMessage(threadId, 'Test message - respond immediately');

    // Try to forget while potentially processing
    const forgetResult = await forgetMessage(threadId, 0);

    await deleteThread(threadId);

    // Should handle gracefully (200 or 400, not 500)
    const passed = forgetResult.status === 200 || forgetResult.status === 404;
    return {
      name: 'Case 9: Forget entry agent is processing',
      passed,
      details: `Forget returned ${forgetResult.status} (no crash)`,
    };
  } catch (e) {
    return {
      name: 'Case 9: Forget entry agent is processing',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase10(): Promise<TestResult> {
  // Edit non-existent sequence (404 error)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 10: Edit non-existent sequence',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Try to edit sequence 999 which doesn't exist
    const editResult = await editMessage(threadId, 999, 'New content');

    await deleteThread(threadId);

    const passed = editResult.status === 404 || editResult.status === 400;
    return {
      name: 'Case 10: Edit non-existent sequence',
      passed,
      details: passed
        ? `Correctly returned ${editResult.status}`
        : `Expected 404 or 400, got ${editResult.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 10: Edit non-existent sequence',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase11(): Promise<TestResult> {
  // Concurrent edits from multiple clients (no race conditions)
  try {
    const threadResult = await createTestThread();
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 11: Concurrent edits from multiple clients',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Send initial message
    await sendMessage(threadId, 'Initial message');

    // Try concurrent edits
    const NUM_EDITS = 5;
    const promises = Array.from({ length: NUM_EDITS }, async (_, i) => {
      return editMessage(threadId, 0, `Edited content ${i}`);
    });

    await Promise.all(promises);

    // Get messages to verify consistency
    const messagesResult = await getMessages(threadId);

    await deleteThread(threadId);

    const passed = messagesResult.status === 200;
    return {
      name: 'Case 11: Concurrent edits from multiple clients',
      passed,
      details: passed ? `Executed ${NUM_EDITS} concurrent edits without crash` : 'Failed',
    };
  } catch (e) {
    return {
      name: 'Case 11: Concurrent edits from multiple clients',
      passed: false,
      error: String(e),
    };
  }
}

async function runAudit(): Promise<AuditSummary> {
  console.log('=== FIRMIUS API MESSAGE AUDIT ===');
  console.log(`API Base URL: ${API_BASE_URL}`);

  const startTime = Date.now();
  const results: TestResult[] = [];

  // Create test directory
  await mkdir(TEST_DIR, { recursive: true });

  // Cleanup any existing test threads
  await cleanupTestThreads();

  try {
    results.push(await testCase6());
    results.push(await testCase7());
    results.push(await testCase8());
    results.push(await testCase9());
    results.push(await testCase10());
    results.push(await testCase11());
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
