/**
 * Validation middleware for Firmius API
 * Request body validation using Zod schemas
 */

import { z } from "zod";
import { Errors } from "./error";
import { access, stat } from "node:fs/promises";
import { resolve, normalize } from "node:path";
import { AgentWorkType } from "@firmius/shared/types";

type BunRequest = Request & {
  method: string;
  url: string;
  headers: Headers;
  // Attach parsed body to request for reuse
  _parsedBody?: unknown;
  // Attach validated threadId for reuse
  _threadId?: string;
  // Attach validated agentId for reuse
  _agentId?: string;
};

type NextFunction = () => Response | Promise<Response>;

// ==================== Path Validation Functions ====================

/**
 * Validates that a path exists and is a directory
 */
async function validateDirectoryPath(
  path: string,
): Promise<{ valid: boolean; error?: string }> {
  try {
    // Resolve absolute path
    const absolutePath = resolve(path);

    // Check for path traversal attempts
    const normalizedPath = normalize(absolutePath);
    if (normalizedPath !== absolutePath) {
      return {
        valid: false,
        error: "Path traversal detected",
      };
    }

    // Check if path exists
    await access(absolutePath);

    // Check if it's a directory
    const stats = await stat(absolutePath);
    if (!stats.isDirectory()) {
      return {
        valid: false,
        error: "Path must be a directory",
      };
    }

    return { valid: true };
  } catch (error) {
    return {
      valid: false,
      error:
        error instanceof Error
          ? error.message
          : "Path does not exist or is not accessible",
    };
  }
}

// ==================== Zod Schemas ====================

/**
 * Schema for generation options
 */
const GenerationOptionsSchema = z
  .object({
    maxTokens: z.number().min(1).max(128000).optional(),
    temperature: z.number().min(0).max(2).optional(),
    topP: z.number().min(0).max(1).optional(),
    frequencyPenalty: z.number().min(-2).max(2).optional(),
    presencePenalty: z.number().min(-2).max(2).optional(),
  })
  .strict();

/**
 * Schema for create thread request
 */
export const CreateThreadRequestSchema = z
  .object({
    hostConfig: z.record(z.string(), z.unknown()),
    rootCwd: z.string().min(1),
    purpose: z.string().min(1),
    objective: z.string().optional(),
    workType: z.nativeEnum(AgentWorkType),
    generationOptions: GenerationOptionsSchema.optional(),
  })
  .strict();

/**
 * Schema for message request
 */
export const MessageRequestSchema = z
  .object({
    message: z.string().min(1).max(1048576), // 1MB max
  })
  .strict();

/**
 * Schema for edit message request
 */
export const EditMessageRequestSchema = z
  .object({
    sequence: z.number().int().min(0),
    newContent: z.string().min(1).max(100000),
  })
  .strict();

/**
 * Schema for thread ID parameter
 */
export const ThreadIdSchema = z.object({
  threadId: z.string().uuid(),
});

/**
 * Schema for agent ID parameter
 */
export const AgentIdSchema = z.object({
  agentId: z.string().uuid(),
});

/**
 * Schema for query parameters (limit and offset)
 */
export const PaginationSchema = z.object({
  limit: z.coerce.number().int().min(1).max(100).default(50),
  offset: z.coerce.number().int().min(0).default(0),
});

// ==================== Validation Helpers ====================

/**
 * Parse and validate JSON body
 */
async function parseJSONBody(req: BunRequest): Promise<unknown> {
  // Return cached body if already parsed
  if (req._parsedBody !== undefined) {
    return req._parsedBody;
  }

  const contentType = req.headers.get("content-type");
  if (!contentType || !contentType.includes("application/json")) {
    throw Errors.badRequest("Content-Type must be application/json");
  }

  try {
    const body = await req.json();
    // Cache the parsed body for reuse
    req._parsedBody = body;
    return body;
  } catch (error) {
    throw Errors.badRequest("Invalid JSON body");
  }
}

/**
 * Validate body against schema and throw if invalid
 */
function validateSchema<T>(schema: z.ZodSchema<T>, data: unknown): T {
  const result = schema.safeParse(data);

  if (!result.success) {
    const errors = result.error.issues.map((err) => ({
      path: err.path.join("."),
      message: err.message,
    }));

    throw Errors.badRequest("Validation failed", { errors });
  }

  return result.data;
}

// ==================== Middleware Functions ====================

/**
 * Generic validation middleware
 * Validates request body against provided schema
 */
export function validateBody<T>(schema: z.ZodSchema<T>) {
  return async (req: BunRequest, next: NextFunction): Promise<Response> => {
    const body = await parseJSONBody(req);
    validateSchema(schema, body);
    return next();
  };
}

/**
 * Validate create thread request
 */
export async function validateCreateThreadRequest(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  // First validate the body structure
  const validationResult = await validateBody(CreateThreadRequestSchema)(
    req,
    next,
  );
  if (
    !(validationResult instanceof Response) ||
    validationResult.status !== 200
  ) {
    return validationResult;
  }

  // Then validate that rootCwd exists and is a directory
  const body = req._parsedBody as {
    rootCwd: string;
    hostConfig?: { type?: string };
  };

  // Only validate path existence for local host (default)
  const isLocal = !body.hostConfig?.type || body.hostConfig.type === "local";

  if (isLocal) {
    const pathValidation = await validateDirectoryPath(body.rootCwd);

    if (!pathValidation.valid) {
      throw Errors.badRequest(`Invalid rootCwd: ${pathValidation.error}`);
    }
  }

  return next();
}

/**
 * Validate message request
 */
export async function validateMessageRequest(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  return validateBody(MessageRequestSchema)(req, next);
}

/**
 * Validate edit message request
 */
export async function validateEditMessageRequest(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  return validateBody(EditMessageRequestSchema)(req, next);
}

/**
 * Validate and extract thread ID from URL
 */
export async function validateThreadId(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  const url = new URL(req.url);
  const pathParts = url.pathname.split("/");

  // Thread ID should be at index 3 (/api/threads/:threadId/...)
  // For /api/threads/:threadId it's at index 3
  // For /api/threads/:threadId/messages it's at index 3
  // For /api/threads/:threadId/events it's at index 3
  const threadId = pathParts[3];

  if (!threadId) {
    throw Errors.badRequest("Thread ID is required");
  }

  try {
    validateSchema(ThreadIdSchema, { threadId });
    // Store validated threadId for reuse
    req._threadId = threadId;
    return next();
  } catch (error) {
    throw Errors.badRequest("Invalid thread ID format");
  }
}

/**
 * Validate and extract agent ID from URL
 */
export async function validateAgentId(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  const url = new URL(req.url);
  const pathParts = url.pathname.split("/");

  // Agent ID can be at different positions depending on route:
  // - /api/agents/:agentId -> index 3
  // - /api/threads/:threadId/agents/:agentId/history -> index 5
  // - /api/threads/:threadId/agents/:agentId/undo-turn -> index 5
  // - /api/threads/:threadId/agents/:agentId/model -> index 5
  let agentId: string | undefined;

  if (pathParts[2] === "agents") {
    // /api/agents/:agentId
    agentId = pathParts[3];
  } else if (pathParts[4] === "agents") {
    // /api/threads/:threadId/agents/:agentId/...
    agentId = pathParts[5];
  }

  if (!agentId) {
    throw Errors.badRequest("Agent ID is required");
  }

  try {
    validateSchema(AgentIdSchema, { agentId });
    // Store validated agentId for reuse
    req._agentId = agentId;
    return next();
  } catch (error) {
    throw Errors.badRequest("Invalid agent ID format");
  }
}

/**
 * Validate query parameters
 */
export async function validateQueryParams(
  req: BunRequest,
  next: NextFunction,
): Promise<Response> {
  const url = new URL(req.url);
  const params = Object.fromEntries(url.searchParams);

  try {
    validateSchema(PaginationSchema, params);
    return next();
  } catch (error) {
    throw Errors.badRequest("Invalid query parameters");
  }
}
