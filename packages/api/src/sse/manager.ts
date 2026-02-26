import type { SSEMessage } from "@firmius/shared/sse";

interface Client {
  id: string;
  response: Response;
  writer: WritableStreamDefaultWriter<Uint8Array>;
  threadId: string;
  abortController: AbortController;
  keepAliveInterval: NodeJS.Timeout;
}

export class SSEManager {
  private readonly clients: Map<string, Client[]> = new Map();

  addClient(
    threadId: string,
    response: Response,
    writer: WritableStreamDefaultWriter<Uint8Array>,
    request: Request,
  ): string {
    const clientId = crypto.randomUUID();
    const abortController = new AbortController();
    const encoder = new TextEncoder();

    const client: Client = {
      id: clientId,
      response,
      writer,
      threadId,
      abortController,
      keepAliveInterval: null as unknown as NodeJS.Timeout,
    };

    if (!this.clients.has(threadId)) {
      this.clients.set(threadId, []);
    }

    this.clients.get(threadId)!.push(client);

    if (request.signal.aborted) {
      this.removeClient(threadId, clientId);
      return clientId;
    }

    request.signal.addEventListener("abort", () => {
      this.removeClient(threadId, clientId);
    });

    const sendKeepAlive = () => {
      if (!abortController.signal.aborted) {
        try {
          writer.write(encoder.encode(": keep-alive\n\n"));
        } catch {
          this.removeClient(threadId, clientId);
        }
      }
    };

    client.keepAliveInterval = setInterval(sendKeepAlive, 30000);

    abortController.signal.addEventListener("abort", () => {
      clearInterval(client.keepAliveInterval);
    });

    return clientId;
  }

  removeClient(threadId: string, clientId: string): void {
    const threadClients = this.clients.get(threadId);
    if (!threadClients) {
      return;
    }

    const clientIndex = threadClients.findIndex((c) => c.id === clientId);
    if (clientIndex === -1) {
      return;
    }

    const client = threadClients[clientIndex]!;

    // Clear keep-alive interval if it exists
    if (client.keepAliveInterval) {
      clearInterval(client.keepAliveInterval);
    }

    // Abort the abort controller
    try {
      client.abortController.abort();
    } catch {
      // Ignore errors from aborting
    }

    // Close writer - handle double-close gracefully
    try {
      client.writer.close();
    } catch {
      // Writer might already be closed
    }

    threadClients.splice(clientIndex, 1);

    if (threadClients.length === 0) {
      this.clients.delete(threadId);
    }
  }

  broadcast(threadId: string, event: SSEMessage): void {
    const threadClients = this.clients.get(threadId);
    if (!threadClients || threadClients.length === 0) {
      return;
    }

    const encoder = new TextEncoder();
    const eventData = JSON.stringify(event);
    const sseMessage = `data: ${eventData}\n\n`;

    const clientsToRemove: string[] = [];

    for (const client of threadClients) {
      if (client.abortController.signal.aborted) {
        clientsToRemove.push(client.id);
        continue;
      }

      try {
        client.writer.write(encoder.encode(sseMessage)).catch(() => {
          // Write failed, mark for removal
          if (!clientsToRemove.includes(client.id)) {
            clientsToRemove.push(client.id);
          }
        });
      } catch {
        if (!clientsToRemove.includes(client.id)) {
          clientsToRemove.push(client.id);
        }
      }
    }

    for (const clientId of clientsToRemove) {
      this.removeClient(threadId, clientId);
    }
  }

  getClientCount(threadId: string): number {
    const threadClients = this.clients.get(threadId);
    return threadClients ? threadClients.length : 0;
  }

  clearThread(threadId: string): void {
    const threadClients = this.clients.get(threadId);
    if (!threadClients) {
      return;
    }

    for (const client of threadClients) {
      clearInterval(client.keepAliveInterval);
      client.abortController.abort();

      try {
        client.writer.close();
      } catch {}
    }

    this.clients.delete(threadId);
  }

  getTotalClientCount(): number {
    let total = 0;
    for (const threadClients of this.clients.values()) {
      total += threadClients.length;
    }
    return total;
  }
}

export default new SSEManager();
