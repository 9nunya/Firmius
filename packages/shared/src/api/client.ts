import { APIError } from "./error";
import type {
  Thread,
  ThreadResponse,
  Message,
  Agent,
  ProviderInfo,
  CreateThreadRequest,
  MessageRequest,
  EditMessageRequest,
  BranchThreadRequest,
  ThreadGenerationOptions,
  UserConfig,
  SSHConfig,
  ThreadEventsResponse,
} from "./types";

export interface APIClientOptions {
  baseUrl?: string;
  fetch?: typeof fetch;
}

const DEFAULT_BASE_URL = "http://localhost:9817";

export class APIClient {
  private baseUrl: string;
  private fetchFn: (input: string | Request, init?: RequestInit) => Promise<Response>;

  constructor(options: APIClientOptions = {}) {
    this.baseUrl = (options.baseUrl ?? DEFAULT_BASE_URL).replace(/\/$/, "");
    if (options.fetch) {
      this.fetchFn = options.fetch;
    } else if (typeof window !== "undefined") {
      this.fetchFn = (...args: Parameters<typeof fetch>) => window.fetch(...args);
    } else {
      this.fetchFn = fetch;
    }
  }

  private getBaseUrl(): string {
    if (typeof window !== "undefined") {
      const port = window.location.port || "9817";
      return `http://${window.location.hostname}:${port}`;
    }
    return this.baseUrl;
  }

  private async request<T>(
    path: string,
    options: RequestInit = {}
  ): Promise<T> {
    // Add cache-busting query param for GET requests (iOS PWA fix)
    const cacheBuster = `__cb__=${Date.now()}`;
    const separator = path.includes('?') ? '&' : '?';
    const url = options.method === 'GET' || !options.method
      ? `${this.getBaseUrl()}${path}${separator}${cacheBuster}`
      : `${this.getBaseUrl()}${path}`;

    const response = await this.fetchFn(url, {
      ...options,
      headers: {
        "Content-Type": "application/json",
        "Cache-Control": "no-cache, no-store, must-revalidate",
        "Pragma": "no-cache",
        ...options.headers,
      },
    });

    if (!response.ok) {
      let errorData: { error: string; details?: unknown } | undefined;
      try {
        errorData = await response.json();
      } catch {
        errorData = { error: response.statusText };
      }
      throw APIError.fromResponse(response, errorData);
    }

    if (response.status === 204) {
      return {} as T;
    }

    return response.json();
  }

  async getThreads(): Promise<ThreadResponse[]> {
    const data = await this.request<{ threads: ThreadResponse[] }>("/api/threads");
    return data.threads ?? [];
  }

  async getThread(id: string): Promise<Thread> {
    return this.request<Thread>(`/api/threads/${id}`);
  }

  async createThread(data: CreateThreadRequest): Promise<Thread> {
    return this.request<Thread>("/api/threads", {
      method: "POST",
      body: JSON.stringify(data),
    });
  }

  async deleteThread(id: string): Promise<void> {
    await this.request<void>(`/api/threads/${id}`, {
      method: "DELETE",
    });
  }

  async interruptThread(id: string): Promise<void> {
    await this.request<void>(`/api/threads/${id}/interrupt`, {
      method: "POST",
    });
  }

  async resumeThread(id: string): Promise<void> {
    await this.request<void>(`/api/threads/${id}/resume`, {
      method: "POST",
    });
  }

  async getMessages(threadId: string): Promise<Message[]> {
    const data = await this.request<{ messages: Message[] }>(
      `/api/threads/${threadId}/messages`
    );
    return data.messages ?? [];
  }

  async sendMessage(threadId: string, content: string | unknown[]): Promise<Message> {
    const request: MessageRequest = { message: content };
    return this.request<Message>(`/api/threads/${threadId}/messages`, {
      method: "POST",
      body: JSON.stringify(request),
    });
  }

  async editMessage(
    threadId: string,
    sequence: number,
    newContent: string
  ): Promise<Message> {
    const request: EditMessageRequest = { sequence, newContent };
    return this.request<Message>(
      `/api/threads/${threadId}/messages/${sequence}/edit`,
      {
        method: "POST",
        body: JSON.stringify(request),
      }
    );
  }

  async forgetMessage(threadId: string, sequence: number): Promise<void> {
    await this.request<void>(
      `/api/threads/${threadId}/messages/${sequence}/forget`,
      {
        method: "POST",
        body: JSON.stringify({ sequence }),
      }
    );
  }

  async unforgetMessage(threadId: string, sequence: number): Promise<void> {
    await this.request<void>(
      `/api/threads/${threadId}/messages/${sequence}/unforget`,
      {
        method: "POST",
        body: JSON.stringify({ sequence }),
      }
    );
  }

  async branchThread(
    threadId: string,
    sequence: number,
    newContent: string
  ): Promise<void> {
    const request: BranchThreadRequest = { sequence, newContent };
    await this.request<void>(
      `/api/threads/${threadId}/messages/${sequence}/branch`,
      {
        method: "POST",
        body: JSON.stringify(request),
      }
    );
  }

  async getAgents(threadId: string): Promise<Agent[]> {
    const data = await this.request<{ agents: Agent[] }>(
      `/api/threads/${threadId}/agents`
    );
    return data.agents ?? [];
  }

  async getProviders(): Promise<ProviderInfo[]> {
    const data = await this.request<{ providers: ProviderInfo[] }>(
      "/api/providers"
    );
    return data.providers ?? [];
  }

  async getSSHConfigs(): Promise<SSHConfig[]> {
    const data = await this.request<{ configs: SSHConfig[] }>("/api/ssh-configs");
    return data.configs ?? [];
  }

  async updateThreadSettings(
    threadId: string,
    settings: ThreadGenerationOptions
  ): Promise<void> {
    await this.request<void>(`/api/threads/${threadId}/settings`, {
      method: "POST",
      body: JSON.stringify({ generationOptions: settings }),
    });
  }

  async getUserConfig(): Promise<UserConfig> {
    return this.request<UserConfig>("/api/user/config");
  }

  async updateUserConfig(config: UserConfig): Promise<UserConfig> {
    return this.request<UserConfig>("/api/user/config", {
      method: "POST",
      body: JSON.stringify(config),
    });
  }

  async getPurposes(): Promise<string[]> {
    return this.request<string[]>("/api/purposes");
  }

  async getThreadEvents(threadId: string): Promise<ThreadEventsResponse> {
    const response = await this.request<ThreadEventsResponse>(
      `/api/threads/${threadId}/events`
    );
    return {
      events: response.events ?? [],
      agents: response.agents ?? [],
    };
  }
}

export const client = new APIClient();
