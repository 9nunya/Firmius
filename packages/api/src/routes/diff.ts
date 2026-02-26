/**
 * Diff routes for Firmius API
 * REST endpoints for retrieving file edit diffs
 */

import type { BunRequest } from "../types/request";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { validateThreadId, validateAgentId } from "../middleware/validation";
import { snapshotStorage } from "@firmius/core/services/SnapshotStorage";

interface DiffHunk {
  oldStart: number;
  oldLines: number;
  newStart: number;
  newLines: number;
  lines: string[];
}

interface DiffResult {
  hunks: DiffHunk[];
  oldFile: string;
  newFile: string;
  stats: {
    additions: number;
    deletions: number;
    changes: number;
  };
  diagnostics?: Array<{
    severity?: number;
    message: string;
    range?: {
      start: { line: number; character: number };
      end: { line: number; character: number };
    };
  }>;
}

/**
 * Generates unified diff from before and after content
 */
function generateDiff(before: string, after: string, fileName: string): DiffResult {
  const beforeLines = before.split("\n");
  const afterLines = after.split("\n");
  
  const hunks: DiffHunk[] = [];
  let additions = 0;
  let deletions = 0;
  
  // Simple line-by-line diff
  let oldLine = 1;
  let newLine = 1;
  let currentHunk: DiffHunk | null = null;
  
  const maxLen = Math.max(beforeLines.length, afterLines.length);
  
  for (let i = 0; i < maxLen; i++) {
    const oldContent = beforeLines[i];
    const newContent = afterLines[i];
    
    if (oldContent !== newContent) {
      // Start a new hunk if needed
      if (!currentHunk) {
        currentHunk = {
          oldStart: oldLine,
          oldLines: 0,
          newStart: newLine,
          newLines: 0,
          lines: [],
        };
      }
      
      if (oldContent !== undefined) {
        currentHunk.lines.push(`-${oldContent}`);
        currentHunk.oldLines++;
        deletions++;
      }
      
      if (newContent !== undefined) {
        currentHunk.lines.push(`+${newContent}`);
        currentHunk.newLines++;
        additions++;
      }
    } else {
      // Same content - close current hunk if exists
      if (currentHunk) {
        hunks.push(currentHunk);
        currentHunk = null;
      }
      
      if (oldContent !== undefined) {
        oldLine++;
      }
      if (newContent !== undefined) {
        newLine++;
      }
    }
  }
  
  // Don't forget the last hunk
  if (currentHunk) {
    hunks.push(currentHunk);
  }
  
  return {
    hunks,
    oldFile: fileName,
    newFile: fileName,
    stats: {
      additions,
      deletions,
      changes: additions + deletions,
    },
  };
}

/**
 * GET /api/threads/:threadId/agents/:agentId/turns/:turnIndex/tools/:toolCallId/diff
 * Retrieve diff for a specific file edit tool call
 */
export const getToolDiff = asyncHandler(async (req: BunRequest) => {
  await validateThreadId(req, async () => new Response());
  await validateAgentId(req, async () => new Response());
  
  const threadId = req._threadId!;
  const agentId = req._agentId!;
  
  // Parse turn index and tool call ID from URL
  const url = new URL(req.url);
  const pathParts = url.pathname.split("/");
  const turnIndexStr = pathParts[pathParts.indexOf("turns") + 1];
  const toolCallId = pathParts[pathParts.indexOf("tools") + 1];
  
  if (!turnIndexStr || !toolCallId) {
    throw Errors.badRequest("Missing turn index or tool call ID");
  }
  
  const turnIdx = parseInt(turnIndexStr, 10);
  if (isNaN(turnIdx) || turnIdx < 0) {
    throw Errors.badRequest("Invalid turn index");
  }
  
  // Retrieve snapshot
  const snapshot = await snapshotStorage.getSnapshot(
    threadId,
    agentId,
    turnIdx,
    toolCallId
  );
  
  if (!snapshot) {
    throw Errors.notFound("Diff not found for this tool call");
  }
  
  // Generate diff from snapshot
  const before = snapshot.beforeContent || "";
  const after = snapshot.afterContent;
  
  const diffResult = generateDiff(before, after, snapshot.file);
  
  // Add diagnostics if present
  if (snapshot.diagnostics) {
    diffResult.diagnostics = snapshot.diagnostics.map(d => ({
      severity: d.severity,
      message: d.message,
      range: d.range,
    }));
  }
  
  return withCORS(
    new Response(JSON.stringify(diffResult), {
      status: 200,
      headers: { "Content-Type": "application/json" },
    }),
    req
  );
});
