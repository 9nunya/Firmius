import type { APIErrorResponse } from "./types";

export class APIError extends Error implements APIErrorResponse {
  error: string;
  details?: unknown;
  statusCode?: number;

  constructor(error: string, details?: unknown, statusCode?: number) {
    super(error);
    this.name = "APIError";
    this.error = error;
    this.details = details;
    this.statusCode = statusCode;
  }

  static fromResponse(response: Response, data?: APIErrorResponse): APIError {
    const error = data?.error ?? response.statusText;
    const details = data?.details;
    return new APIError(error, details, response.status);
  }

  static fromError(error: unknown): APIError {
    if (error instanceof APIError) {
      return error;
    }
    return new APIError(
      error instanceof Error ? error.message : "Unknown error occurred",
      error
    );
  }

  toJSON(): APIErrorResponse & { statusCode?: number } {
    return {
      error: this.error,
      details: this.details,
      statusCode: this.statusCode,
    };
  }
}

export function isAPIError(value: unknown): value is APIError {
  return value instanceof APIError;
}
