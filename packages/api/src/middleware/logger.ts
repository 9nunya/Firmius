/**
 * Logger middleware for Firmius API
 * Logs request/response details with timing
 */

type BunRequest = Request & {
  method: string;
  url: string;
  headers: Headers;
};

type NextFunction = () => Response | Promise<Response>;

const DEBUG_MODE = process.env.DEBUG === 'true';

/**
 * Format timestamp to ISO string
 */
function getTimestamp(): string {
  return new Date().toISOString();
}

/**
 * Extract request details for logging
 */
function getRequestDetails(req: BunRequest): string {
  const { method, url } = req;
  const userAgent = req.headers.get('user-agent') || 'unknown';
  return `[${method}] ${url} - UA: ${userAgent}`;
}

/**
 * Extract request body for debug logging
 */
async function getRequestBody(req: BunRequest): Promise<string> {
  if (!DEBUG_MODE) {
    return '';
  }

  const contentType = req.headers.get('content-type');
  if (!contentType || !contentType.includes('application/json')) {
    return '';
  }

  try {
    const body = await req.clone().text();
    if (body) {
      return ` | Body: ${body.substring(0, 500)}${body.length > 500 ? '...' : ''}`;
    }
  } catch (error) {
    return ' | Body: [unable to read]';
  }

  return '';
}

/**
 * Extract response details for logging
 */
function getResponseDetails(status: number, duration: number): string {
  return `Status: ${status} | Duration: ${duration}ms`;
}

/**
 * Log incoming request
 */
function logRequest(req: BunRequest): void {
  const timestamp = getTimestamp();
  const details = getRequestDetails(req);
  console.log(`[${timestamp}] INCOMING: ${details}`);
}

/**
 * Log outgoing response
 */
function logResponse(req: BunRequest, status: number, duration: number): void {
  const timestamp = getTimestamp();
  const details = getResponseDetails(status, duration);
  const method = req.method;
  const url = new URL(req.url).pathname;
  console.log(`[${timestamp}] OUTGOING: [${method}] ${url} | ${details}`);
}

/**
 * Logger middleware
 * Logs request/response with timing information
 */
export async function logger(req: BunRequest, next: NextFunction): Promise<Response> {
  const startTime = performance.now();
  logRequest(req);

  let body = '';
  if (DEBUG_MODE) {
    body = await getRequestBody(req);
    if (body) {
      console.log(`[${getTimestamp()}] REQUEST BODY: ${body}`);
    }
  }

  const response = await next();

  const endTime = performance.now();
  const duration = Math.round(endTime - startTime);
  logResponse(req, response.status, duration);

  return response;
}
