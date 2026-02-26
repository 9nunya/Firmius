/**
 * Error handling middleware for Firmius API
 * Provides global error handling with JSON responses
 */

import { APIError } from "@firmius/shared/api";

type BunRequest = Request & {
  method: string;
  url: string;
  headers: Headers;
};

type NextFunction = () => Response;

/**
 * API Error class with status code
 */
export class APIException extends Error {
  constructor(
    message: string,
    public statusCode: number = 500,
    public details?: unknown,
  ) {
    super(message);
    this.name = "APIException";
  }
}

/**
 * Common error factories
 */
export const Errors = {
  badRequest: (message: string, details?: unknown): APIException =>
    new APIException(message, 400, details),

  unauthorized: (message: string = "Unauthorized"): APIException =>
    new APIException(message, 401),

  forbidden: (message: string = "Forbidden"): APIException =>
    new APIException(message, 403),

  notFound: (message: string = "Resource not found"): APIException =>
    new APIException(message, 404),

  conflict: (message: string, details?: unknown): APIException =>
    new APIException(message, 409, details),

  unprocessable: (message: string, details?: unknown): APIException =>
    new APIException(message, 422, details),

  internal: (
    message: string = "Internal server error",
    details?: unknown,
  ): APIException => new APIException(message, 500, details),

  serviceUnavailable: (message: string = "Service unavailable"): APIException =>
    new APIException(message, 503),
};

/**
 * Format error response
 */
function formatErrorResponse(
  error: Error | APIException,
  includeStackTrace = false,
): APIError {
  const baseError = {
    error: error.message,
  } as APIError;

  if (error instanceof APIException && error.details) {
    baseError.details = error.details;
  }

  if (includeStackTrace && error.stack) {
    (baseError as any).stack = error.stack;
  }

  return baseError;
}

/**
 * Determine status code from error
 */
function getStatusCode(error: Error | APIException): number {
  if (error instanceof APIException) {
    return error.statusCode;
  }

  if (error.message.toLowerCase().includes("not found")) {
    return 404;
  }

  if (error.message.toLowerCase().includes("unauthorized")) {
    return 401;
  }

  if (error.message.toLowerCase().includes("forbidden")) {
    return 403;
  }

  if (error.message.toLowerCase().includes("invalid")) {
    return 400;
  }

  return 500;
}

/**
 * Log error details
 */
function logError(error: Error | APIException, req: BunRequest): void {
  const timestamp = new Date().toISOString();
  const method = req.method;
  const url = new URL(req.url).pathname;
  const statusCode = getStatusCode(error);

  console.error(
    `[${timestamp}] ERROR: [${method}] ${url} | Status: ${statusCode} | Message: ${error.message}`,
  );

  if (process.env.NODE_ENV === "development" && error.stack) {
    console.error("Stack trace:", error.stack);
  }
}

/**
 * Create JSON error response
 */
function createErrorResponse(error: Error | APIException): Response {
  const statusCode = getStatusCode(error);
  const includeStackTrace = process.env.NODE_ENV === "development";
  const errorBody = formatErrorResponse(error, includeStackTrace);

  return new Response(JSON.stringify(errorBody), {
    status: statusCode,
    headers: {
      "Content-Type": "application/json",
    },
  });
}

/**
 * Error handler middleware
 * Catches all errors and returns JSON responses
 */
export function errorHandler(req: BunRequest, next: NextFunction): Response {
  try {
    return next();
  } catch (error) {
    if (error instanceof Error) {
      logError(error, req);
      return createErrorResponse(error);
    }

    const unknownError = new Error("Unknown error occurred");
    logError(unknownError, req);
    return createErrorResponse(unknownError);
  }
}

/**
 * Async wrapper for route handlers
 * Catches errors in async functions
 */
export function asyncHandler<T extends BunRequest>(
  handler: (req: T) => Promise<Response>,
): (req: T) => Promise<Response> {
  return async (req: T): Promise<Response> => {
    try {
      return await handler(req);
    } catch (error) {
      if (error instanceof Error) {
        logError(error, req);
        return createErrorResponse(error);
      }

      const unknownError = new Error("Unknown error occurred");
      logError(unknownError, req);
      return createErrorResponse(unknownError);
    }
  };
}
