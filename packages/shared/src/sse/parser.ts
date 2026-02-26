import type { EngineEvent } from "../types/engine/IEngine";

/**
 * SSE Message Parser - Direct pass-through for aligned architecture.
 */
export type ParsedEvent = EngineEvent & {
  // Frontend-specific date conversion
  timestamp: Date;
};

function parseTimestamp(timestamp: string | Date | number | undefined): Date {
  if (!timestamp) return new Date();
  if (timestamp instanceof Date) return timestamp;
  if (typeof timestamp === 'number') return new Date(timestamp);
  const parsed = new Date(timestamp);
  return isNaN(parsed.getTime()) ? new Date() : parsed;
}

/**
 * Normalizes an SSE message (EngineEvent) into a ParsedEvent.
 * This is now a simple pass-through with timestamp conversion.
 */
export function parseEvent(message: any): ParsedEvent | null {
  if (!message || typeof message.type !== 'string') return null;

  return {
    ...message,
    timestamp: parseTimestamp(message.timestamp)
  } as ParsedEvent;
}

export const parseSSEMessage = parseEvent;

export function parseRawSSEData(data: string): any | null {
  try {
    return JSON.parse(data);
  } catch {
    return null;
  }
}

export function parseSSEPart(part: string): string {
  const lines = part.split("\n");
  let data = "";
  for (const line of lines) {
    if (line.startsWith("data: ")) {
      data += line.slice(6);
    }
  }
  return data;
}
