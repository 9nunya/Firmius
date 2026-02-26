import { expect, test, describe, beforeEach } from "bun:test";
import { FileReadTool } from "../src/tools/FileTools";
import { HostFactory } from "../src/HostFactory";
import { HostType } from "@firmius/shared";

describe("FileReadTool", () => {
    let context: any;
    const testFile = "test_read.txt";
    const testContent = "Line 1\nLine 2\nLine 3\nLine 4\nLine 5";

    beforeEach(async () => {
        const host = await HostFactory.create({ type: HostType.Local });
        await host.writeFile(testFile, testContent);

        // Minimal mock context
        context = {
            host,
            agent: {
                environment: {
                    cwd: process.cwd(),
                    permissions: {
                        scopes: ["filesystem:read"],
                        allowOutsideCwd: true
                    }
                },
                execution: {
                    maxContextChars: 10000
                }
            }
        };
    });

    test("should read entire file if no range specified", async () => {
        const result = await FileReadTool.execute({ file: testFile }, context as any);
        expect(result.success).toBe(true);
        expect(result.content).toBe(testContent);
        expect(result.totalLines).toBe(5);
    });

    test("should read specific line range (L2-L4)", async () => {
        const result = await FileReadTool.execute(
            { file: testFile, startLine: 2, endLine: 4 },
            context as any
        );
        expect(result.success).toBe(true);
        expect(result.content).toBe("Line 2\nLine 3\nLine 4");
        expect(result.totalLines).toBe(5);
    });

    test("should handle single line (L3)", async () => {
        const result = await FileReadTool.execute(
            { file: testFile, startLine: 3, endLine: 3 },
            context as any
        );
        expect(result.success).toBe(true);
        expect(result.content).toBe("Line 3");
    });

    test("should handle out of bounds gracefully", async () => {
        const result = await FileReadTool.execute(
            { file: testFile, startLine: 0, endLine: 10 },
            context as any
        );
        expect(result.success).toBe(true);
        expect(result.content).toBe(testContent);
    });
});
