import { z } from "zod";
import { type ITool, ToolScope, type ToolResult } from "@firmius/shared/types";
import TurndownService from "turndown";

const turndownService = new TurndownService();

export interface WebSearchResult {
  operation: "search";
  result: string;
  citations: string[];
}

export interface WebFetchResult {
  operation: "fetch";
  content: string;
}

export type WebAccessOutput = WebSearchResult | WebFetchResult;

export interface WebAccessInputSearch {
  operation: "search";
  query: string;
}

export interface WebAccessInputFetch {
  operation: "fetch";
  url: string;
}

export type WebAccessInput = WebAccessInputSearch | WebAccessInputFetch;

const searchSchema = z.object({
  operation: z.literal("search"),
  query: z.string().describe("The search query. Be specific.")
});

const fetchSchema = z.object({
  operation: z.literal("fetch"),
  url: z.string().url().describe("The URL to fetch.")
});

export const webAccessInputSchema = z.discriminatedUnion("operation", [
  searchSchema,
  fetchSchema
]);

async function executeSearch(query: string): Promise<ToolResult<WebAccessOutput>> {
  const apiKey = process.env.PERPLEXITY_API_KEY;
  if (!apiKey) {
    return {
      success: false,
      summary: "Perplexity key missing",
      error: "PERPLEXITY_API_KEY not configured in environment."
    };
  }

  try {
    const response = await fetch("https://api.perplexity.ai/chat/completions", {
      method: "POST",
      headers: {
        "Authorization": `Bearer ${apiKey}`,
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        model: "sonar-pro",
        messages: [
          { role: "system", content: "You are a helpful research assistant. Provide accurate, well-sourced answers with citations." },
          { role: "user", content: query }
        ]
      })
    });

    if (!response.ok) {
      const errorText = await response.text();
      return {
        success: false,
        summary: "Search API error",
        error: `API Error: ${response.status} ${response.statusText} - ${errorText}`
      };
    }

    const data = await response.json() as any;
    const content = data.choices[0]?.message?.content || "No result returned.";
    const citations = data.citations || [];

    return {
      success: true,
      summary: `Searched: "${query}"`,
      output: {
        operation: "search",
        result: content,
        citations
      }
    };

  } catch (e: any) {
    return {
      success: false,
      summary: "Search network error",
      error: `Network Error: ${e.message}`
    };
  }
}

async function executeFetch(url: string): Promise<ToolResult<WebAccessOutput>> {
  try {
    const response = await fetch(url, {
      headers: {
        "User-Agent": "Firmius/1.0 (Autonomous Research Agent)"
      }
    });

    if (!response.ok) {
      return {
        success: false,
        summary: "Fetch HTTP error",
        error: `HTTP ${response.status} ${response.statusText}`
      };
    }

    const html = await response.text();

    const cleanHtml = html
      .replace(/<script\b[^>]*>([\s\S]*?)<\/script>/gm, "")
      .replace(/<style\b[^>]*>([\s\S]*?)<\/style>/gm, "")
      .replace(/<svg\b[^>]*>([\s\S]*?)<\/svg>/gm, "[SVG]");

    const markdown = turndownService.turndown(cleanHtml);

    return {
      success: true,
      summary: `Fetched: ${url}`,
      output: {
        operation: "fetch",
        content: markdown
      }
    };
  } catch (e: any) {
    return {
      success: false,
      summary: "Fetch network error",
      error: `Fetch Error: ${e.message}`
    };
  }
}

export const WebAccessTool: ITool<WebAccessInput, WebAccessOutput> = {
  metadata: {
    name: "web_access",
    description: "Access the web.",
    scope: ToolScope.Web
  },
  input: webAccessInputSchema,
  execute: async (input: WebAccessInput): Promise<ToolResult<WebAccessOutput>> => {
    if (input.operation === "search") {
      return executeSearch(input.query);
    } else {
      return executeFetch(input.url);
    }
  },
  summarizeInput: (input: WebAccessInput) => {
    if (input.operation === "search") {
      return `search: "${input.query}"`;
    } else {
      return `fetch: "${input.url}"`;
    }
  },
};

export const AllWebTools = [WebAccessTool];
