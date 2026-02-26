/**
 * Message routes for Firmius API
 * REST endpoints for message operations
 */

import type { BunRequest } from "../types/index";
import { withCORS } from "../middleware/cors";
import { Errors, asyncHandler } from "../middleware/error";
import { validateThreadId, validateMessageRequest, validateEditMessageRequest } from "../middleware/validation";
import type { MessageRequest, EditMessageRequest, BranchThreadRequest } from "@firmius/shared/api";
import type { ProviderMessageContentPart } from "@firmius/shared/types";
import EventService from "../services/EventService";
import ThreadService from "../services/ThreadService";
import StateService from "../services/StateService";
import { snapshotStorage } from "@firmius/core/services/SnapshotStorage";
import { writeFile } from "node:fs/promises";

type RouteHandler = (req: BunRequest) => Promise<Response>;

// ==================== Message Routes ====================

/**
 * POST /api/threads/:id/messages/:seq/branch - Branch thread from message
 */
export const branchThread: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;
    const body = (await req.json()) as BranchThreadRequest;

    const url = new URL(req.url);
    const parts = url.pathname.split("/");
    const seqString = parts[5];
    if (!seqString) {
      throw Errors.badRequest("Missing sequence in URL");
    }
    const sequence = parseInt(seqString, 10);

    if (isNaN(sequence)) {
      throw Errors.badRequest("Invalid sequence number");
    }

    const thread = await ThreadService.getThread(threadId) as any;
    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    // Interrupt active agents
    const activeAgents = [thread.leadAgent, ...thread.getSubagents()].filter(a => a.status === 'working');
    for (const agent of activeAgents) {
      await agent.interrupt();
    }

    // Edit the message
    await StateService.editMessage(thread, sequence, body.newContent);

    // Truncate thread history after this message
    await StateService.forgetEventsAfterSequence(thread, sequence);

    // Emit refresh event
    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "message_added",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          sequence,
          content: body.newContent,
          type: "branch_deleted", // Specialized type to trigger UI cleanup
        } as any,
      ),
    );

    return withCORS(
      new Response(JSON.stringify({ message: "Thread branched successfully" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * GET /api/threads/:id/messages - Get message history
 */
export const getMessages: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId;
    if (!threadId) {
      throw Errors.badRequest("Thread ID is required");
    }

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    const messages = await StateService.getMessageHistory(thread);

    return withCORS(
      new Response(
        JSON.stringify({
          messages,
          count: messages.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/messages - Send message
 */
export const sendMessage: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());
    await validateMessageRequest(req, async () => new Response());

    const threadId = req._threadId;
    if (!threadId) {
      throw Errors.badRequest("Thread ID is required");
    }
    const body = req._parsedBody as MessageRequest;

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    thread.clearInterrupted();

    const message = await StateService.sendMessage(thread, body.message as string | ProviderMessageContentPart[]);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "message_added",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          ...message,
          addedType: "new",
        } as any,
      ),
    );

    // Trigger agent processing asynchronously (fire-and-forget)
    // Agent emits events during execution via Engine -> SSEManager -> frontend
    if (thread.leadAgent) {
      thread.leadAgent.actUntilAgentEnds().catch((error: Error) => {
        console.error("Agent execution failed:", error);
        EventService.addEvent(
          threadId,
          EventService.createSSEEvent(
            "error_occurred",
            threadId,
            thread.leadAgent.id,
            thread.leadAgent.readableName ?? thread.leadAgent.id,
            {
              error: {
                message: error.message,
                stack: error.stack,
              },
              phase: "agent_execution",
            },
          ),
        );
      });
    }

    return withCORS(
      new Response(JSON.stringify(message), {
        status: 201,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/messages/:seq/edit - Edit message
 */
export const editMessage: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());
    await validateEditMessageRequest(req, async () => new Response());

    const threadId = req._threadId!;
    const body = req._parsedBody as EditMessageRequest;

    const url = new URL(req.url);
    const seqString = url.pathname.split("/")[5];

    if (!seqString) {
      throw Errors.badRequest("Sequence number is required");
    }

    const sequence = parseInt(seqString, 10);

    if (isNaN(sequence)) {
      throw Errors.badRequest("Invalid sequence number");
    }

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    await StateService.editMessage(thread, sequence, body.newContent);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "message_added",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          sequence,
          content: body.newContent,
          type: "edit",
        },
      ),
    );

    return withCORS(
      new Response(JSON.stringify({ message: "Message edited successfully" }), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      }),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/messages/:seq/forget - Forget message
 */
export const forgetMessage: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const url = new URL(req.url);
    const seqString = url.pathname.split("/")[5];

    if (!seqString) {
      throw Errors.badRequest("Sequence number is required");
    }

    const sequence = parseInt(seqString, 10);

    if (isNaN(sequence)) {
      throw Errors.badRequest("Invalid sequence number");
    }

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    await StateService.forgetEntry(thread, sequence);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "agent_status_changed",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          status: "idle",
          reason: `Message ${sequence} forgotten`,
        },
      ),
    );

    return withCORS(
      new Response(
        JSON.stringify({ message: "Message forgotten successfully" }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * POST /api/threads/:id/messages/:seq/unforget - Unforget message
 */
export const unforgetMessage: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const url = new URL(req.url);
    const seqString = url.pathname.split("/")[5];

    if (!seqString) {
      throw Errors.badRequest("Sequence number is required");
    }

    const sequence = parseInt(seqString, 10);

    if (isNaN(sequence)) {
      throw Errors.badRequest("Invalid sequence number");
    }

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    await StateService.unforgetEntry(thread, sequence);

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "agent_status_changed",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          status: "idle",
          reason: `Message ${sequence} restored`,
        },
      ),
    );

    return withCORS(
      new Response(
        JSON.stringify({ message: "Message restored successfully" }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);

/**
 * Routes dispatcher for /api/threads/:id/messages
 */
export function messageRoutes(req: BunRequest): Response | Promise<Response> {
  const url = new URL(req.url);
  const path = url.pathname;

  const messagesMatch = path.match(/^\/api\/threads\/([^/]+)\/messages$/);
  if (messagesMatch) {
    if (req.method === "GET") {
      return getMessages(req);
    } else if (req.method === "POST") {
      return sendMessage(req);
    }
  }

  const editMatch = path.match(
    /^\/api\/threads\/([^/]+)\/messages\/(\d+)\/edit$/,
  );
  if (editMatch && req.method === "POST") {
    return editMessage(req);
  }

  const branchMatch = path.match(
    /^\/api\/threads\/([^/]+)\/messages\/(\d+)\/branch$/,
  );
  if (branchMatch && req.method === "POST") {
    return branchThread(req);
  }

  const forgetMatch = path.match(
    /^\/api\/threads\/([^/]+)\/messages\/(\d+)\/forget$/,
  );
  if (forgetMatch && req.method === "POST") {
    return forgetMessage(req);
  }

  const unforgetMatch = path.match(
    /^\/api\/threads\/([^/]+)\/messages\/(\d+)\/unforget$/,
  );
  if (unforgetMatch && req.method === "POST") {
    return unforgetMessage(req);
  }

  const undoMatch = path.match(
    /^\/api\/threads\/([^/]+)\/messages\/(\d+)\/undo$/,
  );
  if (undoMatch && req.method === "POST") {
    return undoMessage(req);
  }

  return withCORS(
    new Response(JSON.stringify({ error: "Method not allowed" }), {
      status: 405,
      headers: { "Content-Type": "application/json" },
    }),
    req,
  );
}

/**
 * POST /api/threads/:id/messages/:seq/undo - Undo message and restore files
 */
export const undoMessage: RouteHandler = asyncHandler(
  async (req: BunRequest) => {
    await validateThreadId(req, async () => new Response());

    const threadId = req._threadId!;

    const url = new URL(req.url);
    const seqString = url.pathname.split("/")[5];

    if (!seqString) {
      throw Errors.badRequest("Sequence number is required");
    }

    const sequence = parseInt(seqString, 10);

    if (isNaN(sequence)) {
      throw Errors.badRequest("Invalid sequence number");
    }

    const thread = await ThreadService.getThread(threadId) as any;

    if (!thread) {
      throw Errors.notFound(`Thread with id ${threadId} not found`);
    }

    // Interrupt active agents
    const activeAgents = [thread.leadAgent, ...thread.getSubagents()].filter(a => a.status === 'working');
    for (const agent of activeAgents) {
      await agent.interrupt();
    }

    // Get the target entry to find its timestamp
    const targetEntry = await StateService.getEntry(thread, sequence);
    const targetTimestamp = targetEntry ? targetEntry.timestamp : 0;

    // Get all snapshots for this thread to find file edits after this sequence/timestamp
    const allSnapshots = await snapshotStorage.getAllSnapshotsForThread(threadId);
    const restoredFiles: string[] = [];

    // Find file edits that happened after this sequence and restore them
    for (const snapshot of allSnapshots) {
      // Use timestamp for more reliable comparison since turnIndex might have mismatching units
      if (snapshot.timestamp > targetTimestamp) {
        try {
          if (snapshot.beforeContent !== null) {
            // Restore file to before state
            await writeFile(snapshot.file, snapshot.beforeContent, 'utf8');
            restoredFiles.push(snapshot.file);
          } else {
            // File was created in this edit, so we should delete it
            // but for safety, we just note it for now unless we're sure
            restoredFiles.push(`${snapshot.file} (created - not deleted)`);
          }
        } catch (err) {
          console.error(`Failed to restore file ${snapshot.file}:`, err);
        }
      }
    }

    // Forget entries after this sequence
    await StateService.forgetEventsAfterSequence(thread, sequence);

    // Emit refresh event
    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "message_added",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          sequence,
          type: "undo_deleted",
        } as any,
      ),
    );

    EventService.addEvent(
      thread.id,
      EventService.createSSEEvent(
        "agent_status_changed",
        thread.id,
        thread.leadAgent.id,
        thread.leadAgent.readableName ?? thread.leadAgent.id,
        {
          status: "idle",
          reason: `Undone to message ${sequence}`,
        },
      ),
    );

    return withCORS(
      new Response(
        JSON.stringify({
          message: "Undo successful",
          restoredFiles,
          restoredCount: restoredFiles.length,
        }),
        {
          status: 200,
          headers: { "Content-Type": "application/json" },
        },
      ),
      req,
    );
  },
);
