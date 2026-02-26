import { describe, test, expect, beforeAll, afterAll } from "bun:test";
import type { IHost } from "@firmius/shared";
import { WebAccessTool } from "../../tools/WebTools";
import { createToolContext } from "../test_utils";

const SUITE_NAME = "web";

function shouldRunSuite(): boolean {
  const envSuites = process.env.FIRMUS_TEST_SUITES?.toLowerCase();
  if (!envSuites || envSuites === "all") return true;
  return envSuites.split(",").includes(SUITE_NAME);
}

export function runWebToolsTests(hostName: string, getHost: () => Promise<IHost>) {
    if (!shouldRunSuite()) return;

    describe(`WebTools (${hostName})`, () => {
        let host: IHost;

        beforeAll(async () => {
            host = await getHost();
            await host.init();
        });

        afterAll(async () => {
            await host.destroy();
        });

        // --- Test 1: Web Fetch (Mocked) ---
        test("web_fetch should convert HTML to Markdown", async () => {
            const ctx = createToolContext(host);
            
            // Mock global fetch
            const originalFetch = global.fetch;
            // @ts-ignore
            global.fetch = async (url: any) => {
                return new Response(`
                    <html>
                        <head><title>Test Page</title></head>
                        <body>
                            <h1>Hello World</h1>
                            <p>This is a test.</p>
                            <script>console.log('bad');</script>
                        </body>
                    </html>
                `, { status: 200 });
            };

            const result = await WebAccessTool.execute({ operation: "fetch", url: "https://example.com" }, ctx);
            
            // Restore fetch
            global.fetch = originalFetch;

            expect(result.success).toBe(true);
            // Type narrowing for fetch result
            if (result.success && result.output?.operation === "fetch") {
                // turndown creates "Hello World\n===========" or similar for H1 depending on config, but definitely contains "Hello World"
                expect(result.output.content).toContain("Hello World");
                expect(result.output.content).toContain("This is a test.");
                expect(result.output.content).not.toContain("console.log"); // Script should be removed
            }
        });

        // --- Test 2: Web Search (Integration / Mocked) ---
        test("web_search should handle missing API key gracefully", async () => {
            const ctx = createToolContext(host);
            const originalKey = process.env.PERPLEXITY_API_KEY;
            
            // Explicitly set to undefined
            delete process.env.PERPLEXITY_API_KEY;

            const result = await WebAccessTool.execute({ operation: "search", query: "Test" }, ctx);
            
            if (originalKey) process.env.PERPLEXITY_API_KEY = originalKey; // Restore

            expect(result.success).toBe(false);
            if (!result.success) {
                expect(result.error).toContain("PERPLEXITY_API_KEY");
            }
        });

        // --- Test 3: Live Perplexity Check (Only runs if key exists) ---
        // We inject the key here for testing purposes if not present
        const testKey = process.env.PERPLEXITY_API_KEY || "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";
        
        if (testKey) {
            test("web_search LIVE request", async () => {
                const ctx = createToolContext(host);
                process.env.PERPLEXITY_API_KEY = testKey;
                
                console.log(`[${hostName}] Running LIVE Perplexity Search...`);
                const result = await WebAccessTool.execute({ operation: "search", query: "What is the capital of France?" }, ctx);
                
                if (!result.success) console.error("Live Search Failed:", result.error);

                expect(result.success).toBe(true);
                if (result.success && result.output?.operation === "search") {
                    expect(result.output.result).toBeString();
                    expect(result.output.result.length).toBeGreaterThan(10);
                    expect(result.output.citations).toBeArray();
                }
        }, 10000); // 10s timeout
        } else {
            console.warn(`[${hostName}] Skipping LIVE Perplexity test (No API Key found)`);
        }

        test("summarizeInput and summary", async () => {
            const ctx = createToolContext(host);

            // WebFetchTool
            const fetchInput = { operation: "fetch" as const, url: "https://example.com" };
            expect(WebAccessTool.summarizeInput(fetchInput)).toBe(`fetch: "https://example.com"`);

            const originalFetch = global.fetch;
            // @ts-ignore
            global.fetch = async (url: any) => {
                return new Response('<html><body>Test content</body></html>', { status: 200 });
            };

            const fetchRes = await WebAccessTool.execute(fetchInput, ctx);
            global.fetch = originalFetch;

            const fetchSummary = WebAccessTool.summary!(fetchRes, fetchInput);
            expect(fetchSummary).toContain("https://example.com");

            // WebSearchTool - test with missing key (will fail but should still have summary)
            const searchInput = { operation: "search" as const, query: "Test query" };
            expect(WebAccessTool.summarizeInput(searchInput)).toBe(`search: "Test query"`);

            const originalKey = process.env.PERPLEXITY_API_KEY;
            delete process.env.PERPLEXITY_API_KEY;

            const searchRes = await WebAccessTool.execute(searchInput, ctx);
            if (originalKey) {
                process.env.PERPLEXITY_API_KEY = originalKey;
            }

            expect(searchRes.success).toBe(false);
            const searchSummary = WebAccessTool.summary!(searchRes, searchInput);
            expect(searchSummary).toContain("Perplexity key missing");
        });
    });
}
