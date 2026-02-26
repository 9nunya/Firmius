export type BunRequest = Request & {
  method: string;
  url: string;
  headers: Headers;
  _parsedBody?: unknown;
  _threadId?: string;
  _agentId?: string;
};

export type NextFunction = () => Response | Promise<Response>;