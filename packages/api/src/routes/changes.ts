/**
 * Changes routes for Firmius API
 * REST endpoints for retrieving file change statistics
 */

import type { BunRequest } from "../types/request";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { validateThreadId, validateAgentId } from "../middleware/validation";
import { snapshotStorage } from "@firmius/core/services/SnapshotStorage";
import ThreadService from "../services/ThreadService";

type RouteHandler = (req: BunRequest) => Promise<Response>;

interface FileChange {
  file: string;
  operation: string;
  additions: number;
  deletions: number;
  timestamp: number;
}

interface ChangesResponse {
  additions: number;
  deletions: number;
  files: FileChange[];
  totalFiles: number;
}

/**
 * Calculate line changes between before and after content
 */
function calculateChanges(before: string | null, after: string): { additions: number; deletions: number } {
  if (!before) {
    // New file - all lines are additions
    return { additions: after.split("\n").length, deletions: 0 };
  }

  const beforeLines = before.split("\n");
  const afterLines = after.split("\n");
  
  let additions = 0;
  let deletions = 0;
  
  const maxLen = Math.max(beforeLines.length, afterLines.length);
  
  for (let i = 0; i < maxLen; i++) {
    const beforeLine = beforeLines[i];
    const afterLine = afterLines[i];
    
    if (beforeLine !== afterLine) {
      if (beforeLine !== undefined) deletions++;
      if (afterLine !== undefined) additions++;
    }
  }
  
  return { additions, deletions };
}

/**
 * GET /api/threads/:threadId/agents/:agentId/changes
 * Get file change statistics for an agent (optionally filtered by turn)
 */
export const getAgentChanges: RouteHandler = asyncHandler(async (req: BunRequest) => {
  await validateThreadId(req, async () => new Response());
  await validateAgentId(req, async () => new Response());

  const threadId = req._threadId!;
  const agentId = req._agentId!;

  const url = new URL(req.url);
  const turnIndexParam = url.searchParams.get("turnIndex");
  const turnIndex = turnIndexParam ? parseInt(turnIndexParam, 10) : null;

  const thread = await ThreadService.getThread(threadId);
  if (!thread) {
    throw Errors.notFound(`Thread with id ${threadId} not found`);
  }

  // Get all snapshots for this thread
  const allSnapshots = await snapshotStorage.getAllSnapshotsForThread(threadId);
  
  // Filter by agent and optionally by turn
  let relevantSnapshots = allSnapshots.filter(s => s.agentId === agentId);
  
  if (turnIndex !== null) {
    relevantSnapshots = relevantSnapshots.filter(s => s.turnIndex === turnIndex);
  }

  // Calculate changes for each file
  const fileChanges: FileChange[] = [];
  let totalAdditions = 0;
  let totalDeletions = 0;

  for (const snapshot of relevantSnapshots) {
    const { additions, deletions } = calculateChanges(
      snapshot.beforeContent,
      snapshot.afterContent
    );

    totalAdditions += additions;
    totalDeletions += deletions;

    fileChanges.push({
      file: snapshot.file,
      operation: snapshot.operation,
      additions,
      deletions,
      timestamp: snapshot.timestamp,
    });
  }

  // Sort by timestamp (newest first)
  fileChanges.sort((a, b) => b.timestamp - a.timestamp);

  const response: ChangesResponse = {
    additions: totalAdditions,
    deletions: totalDeletions,
    files: fileChanges,
    totalFiles: fileChanges.length,
  };

  return withCORS(
    new Response(JSON.stringify(response), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    req
  );
});

/**
 * GET /api/threads/:threadId/changes
 * Get all file changes in a thread (for lead agent view)
 */
export const getThreadChanges: RouteHandler = asyncHandler(async (req: BunRequest) => {
  await validateThreadId(req, async () => new Response());

  const threadId = req._threadId!;

  const thread = await ThreadService.getThread(threadId);
  if (!thread) {
    throw Errors.notFound(`Thread with id ${threadId} not found`);
  }

  // Get all snapshots for this thread (all agents)
  const allSnapshots = await snapshotStorage.getAllSnapshotsForThread(threadId);

  // Calculate changes for each file
  const fileChanges: FileChange[] = [];
  let totalAdditions = 0;
  let totalDeletions = 0;

  for (const snapshot of allSnapshots) {
    const { additions, deletions } = calculateChanges(
      snapshot.beforeContent,
      snapshot.afterContent
    );

    totalAdditions += additions;
    totalDeletions += deletions;

    fileChanges.push({
      file: snapshot.file,
      operation: snapshot.operation,
      additions,
      deletions,
      timestamp: snapshot.timestamp,
    });
  }

  // Sort by timestamp (newest first)
  fileChanges.sort((a, b) => b.timestamp - a.timestamp);

  const response: ChangesResponse = {
    additions: totalAdditions,
    deletions: totalDeletions,
    files: fileChanges,
    totalFiles: fileChanges.length,
  };

  return withCORS(
    new Response(JSON.stringify(response), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    req
  );
});
