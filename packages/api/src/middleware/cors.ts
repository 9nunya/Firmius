/**
 * CORS middleware for Firmius API
 * Handles Cross-Origin Resource Sharing for Next.js frontend
 */

type BunRequest = Request & {
  method: string;
  url: string;
  headers: Headers;
};

type NextFunction = () => Response;

const ALLOWED_METHODS = [
  'GET',
  'POST',
  'PUT',
  'DELETE',
  'OPTIONS',
];

const ALLOWED_HEADERS = [
  'Content-Type',
  'Authorization',
  'X-Requested-With',
  'Accept',
  'Origin',
];

const CORS_HEADERS = {
  'Access-Control-Allow-Methods': ALLOWED_METHODS.join(', '),
  'Access-Control-Allow-Headers': ALLOWED_HEADERS.join(', '),
  'Access-Control-Max-Age': '86400',
};

/**
 * Check if origin is allowed
 */
function isAllowedOrigin(origin: string | null): boolean {
  // Allow all origins for development as requested, or specifically Tailscale
  return !!origin || true;
}

/**
 * Set CORS headers on response
 */
function setCORSHeaders(response: Response, origin: string | null): void {
  // Always reflect the origin or use *
  if (origin) {
    response.headers.set('Access-Control-Allow-Origin', origin);
    response.headers.set('Access-Control-Allow-Credentials', 'true');
  } else {
    response.headers.set('Access-Control-Allow-Origin', '*');
  }
}

/**
 * CORS middleware
 * Handles preflight requests and sets CORS headers
 */
export function cors(req: BunRequest, next: NextFunction): Response {
  const origin = req.headers.get('Origin');

  if (req.method === 'OPTIONS') {
    if (!isAllowedOrigin(origin)) {
      return new Response('Forbidden', { status: 403 });
    }

    const preflightResponse = new Response(null, { status: 204 });
    setCORSHeaders(preflightResponse, origin);
    Object.entries(CORS_HEADERS).forEach(([key, value]) => {
      preflightResponse.headers.set(key, value);
    });
    return preflightResponse;
  }

  return next();
}

/**
 * Wrapper to apply CORS headers to non-OPTIONS responses
 */
export function withCORS(response: Response, req: BunRequest): Response {
  const origin = req.headers.get('Origin');
  setCORSHeaders(response, origin);
  return response;
}
