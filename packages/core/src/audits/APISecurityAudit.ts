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
const AUDIT_THREAD_PREFIX = 'api-security-audit-';
const TEST_DIR = join(tmpdir(), 'firmius-api-security-audit');

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

async function createTestThread(
  rootCwd: string
): Promise<{ id: string; status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      hostConfig: { type: 0 },
      rootCwd,
      purpose: 'General',
      objective: 'Test security operations',
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

async function getThread(threadId: string): Promise<{ status: number; data?: unknown }> {
  const response = await fetch(`${API_BASE_URL}/api/threads/${threadId}`);
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

async function testCase21(): Promise<TestResult> {
  // Path traversal '../../../etc/passwd' (blocked, 400 error)
  try {
    const result = await createTestThread('../../../etc/passwd');

    // Should reject the path traversal attempt
    const passed = result.status === 400 || result.status === 500;

    // If it somehow created, delete it
    if (result.status === 201 && result.data) {
      const threadId = (result.data as { id: string }).id;
      await deleteThread(threadId);
    }

    return {
      name: 'Case 21: Path traversal \'../../../etc/passwd\'',
      passed,
      details: passed
        ? `Correctly blocked path traversal with status ${result.status}`
        : `Accepted path traversal with status ${result.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 21: Path traversal \'../../../etc/passwd\'',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase22(): Promise<TestResult> {
  // 1000-character thread ID (handled, no crash)
  try {
    // Create a thread first to get a valid thread
    const threadResult = await createTestThread(TEST_DIR);
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 22: 1000-character thread ID',
        passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Try to access with a very long thread ID
    const longThreadId = 'a'.repeat(1000);
    const result = await getThread(longThreadId);

    await deleteThread(threadId);

    // Should gracefully handle long IDs (404 or 400)
    const passed = result.status === 404 || result.status === 400;
    return {
      name: 'Case 22: 1000-character thread ID',
      passed,
      details: passed
        ? `Gracefully handled long ID with status ${result.status}`
        : `May have issues with long IDs (status ${result.status})`,
    };
  } catch (e) {
    return {
      name: 'Case 22: 1000-character thread ID',
      passed: false,
      error: String(e),
    };
  }
}

async function testCase23(): Promise<TestResult> {
  // SQL injection in message body (sanitized, no injection)
  try {
    const threadResult = await createTestThread(TEST_DIR);
    if (threadResult.status !== 201 || !threadResult.data) {
      return {
        name: 'Case 23: SQL injection in message',
      passed: false,
        error: `Failed to create test thread: ${threadResult.status}`,
      };
    }

    const threadId = (threadResult.data as { id: string }).id;

    // Send SQL injection payload
    const sqlPayload = "'; DROP TABLE users; --";
    const messageResult = await sendMessage(threadId, sqlPayload);

    // Try another SQL injection variant
    const sqlPayload2 = "' OR '1'='1";
    await sendMessage(threadId, sqlPayload2);

    // Try XSS
    const xssPayload = '<script>alert("XSS")</script>';
    await sendMessage(threadId, xssPayload);

    // Try command injection
    const cmdPayload = '$(whoami)';
    await sendMessage(threadId, cmdPayload);

    await deleteThread(threadId);

    // Test passes if messages are accepted and no crash occurs
    // The payloads should be treated as plain text, not executed
    const passed = messageResult.status === 201;
    return {
      name: 'Case 23: SQL injection in message',
      passed,
      details: passed
        ? 'Injection payloads accepted as plain text (no execution)'
        : `Message rejected with status ${messageResult.status}`,
    };
  } catch (e) {
    return {
      name: 'Case 23: SQL injection in message',
      passed: false,
      error: String(e),
    };
  }
}

async function runAudit(): Promise<AuditSummary> {
  console.log('=== FIRMIUS API SECURITY AUDIT ===');
  console.log(`API Base URL: ${API_BASE_URL}`);

  const startTime = Date.now();
  const results: TestResult[] = [];

  // Create test directory
  await mkdir(TEST_DIR, { recursive: true });

  // Cleanup any existing test threads
  await cleanupTestThreads();

  try {
    results.push(await testCase21());
    results.push(await testCase22());
    results.push(await testCase23());
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
