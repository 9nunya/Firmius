import { resolve, relative, join, extname } from "node:path";
import z from "zod";
import picomatch from "picomatch";
import type { ITool, ToolContext, LSPDiagnostic, ToolResult } from "@firmius/shared/types";
import { ToolScope } from "@firmius/shared";
import { Engine } from "@firmius/core";
import { snapshotStorage } from "../services/SnapshotStorage";

// Helper to store file edit snapshot after successful edit
async function storeEditSnapshot(
  context: ToolContext,
  file: string,
  operation: string,
  beforeContent: string | null,
  afterContent: string,
  diagnostics?: LSPDiagnostic[],
  hunks?: Array<{ startLine: number; endLine: number; action: string }>,
): Promise<void> {
  if (!context.toolCallId || !context.threadId) return;

  try {
    await snapshotStorage.storeSnapshot({
      threadId: context.threadId,
      agentId: context.agent.id,
      turnIndex: context.turnIndex || 0,
      toolCallId: context.toolCallId,
      file,
      operation,
      beforeContent,
      afterContent,
      timestamp: Date.now(),
      hunks,
      diagnostics,
    });
  } catch (err) {
    console.error('Failed to store snapshot:', err);
  }
}

const BINARY_EXTENSIONS = new Set([
  ".db",
  ".sqlite",
  ".sqlite3",
  ".bin",
  ".so",
  ".dll",
  ".exe",
  ".dylib",
  ".png",
  ".jpg",
  ".jpeg",
  ".gif",
  ".bmp",
  ".ico",
  ".webp",
  ".svg",
  ".pdf",
  ".zip",
  ".tar",
  ".gz",
  ".rar",
  ".7z",
  ".mp3",
  ".mp4",
  ".wav",
  ".avi",
  ".mov",
  ".webm",
  ".ttf",
  ".otf",
  ".woff",
  ".woff2",
  ".eot",
  ".class",
  ".pyc",
  ".o",
  ".obj",
  ".a",
  ".lib",
  ".DS_Store",
  ".gitignore",
  ".env",
  ".env.local",
  ".env.development",
  ".env.test",
  ".env.production",
]);

function isBinaryFile(filePath: string, content: string): boolean {
  const ext = extname(filePath).toLowerCase();
  if (BINARY_EXTENSIONS.has(ext)) {
    return true;
  }
  const sample = content.slice(0, 512);
  for (let i = 0; i < sample.length; i++) {
    if (sample.charCodeAt(i) === 0) {
      return true;
    }
  }
  return false;
}

function validatePath(
  rawPath: string,
  context: ToolContext,
  mode: "read" | "write" = "read",
): string {
  const cwd = context.agent.environment.cwd
    ? context.agent.environment.cwd.toString()
    : context.host.defaultCwd.toString();
  const absPath = resolve(cwd, rawPath);

  if (!context.agent.environment.permissions.allowOutsideCwd) {
    const rel = relative(cwd, absPath);
    if (rel.startsWith("..") || rel.startsWith("/")) {
      throw new Error(
        `Access denied: Path ${absPath} is outside of CWD ${cwd}`,
      );
    }
  }

  // Add sandboxing check for write operations
  const allowPaths = context.agent.environment.permissions.allowPaths;
  if (mode === "write" && allowPaths && allowPaths.length > 0) {
    const isAllowed = allowPaths.some((pattern) => {
      const absolutePattern = pattern.startsWith("/")
        ? pattern
        : join(cwd, pattern);
      const matcher = picomatch(absolutePattern, { dot: true });
      return matcher(absPath);
    });
    if (!isAllowed) {
      throw new Error(
        `Write access denied: ${rawPath} is outside allowed paths: ${allowPaths.join(", ")}`,
      );
    }
  }

  return absPath;
}

// =============================================================================
// FILE READ TOOL
// =============================================================================

export interface FileReadInput {
  file: string;
  startLine?: number;
  endLine?: number;
}

export interface FileReadOutput {
  content: string;
  totalLines: number;
}

const FileReadInputSchema = z.object({
  file: z.string().describe("The file path to read."),
  startLine: z
    .number()
    .optional()
    .describe("Starting line number (1-indexed)."),
  endLine: z.number().optional().describe("Ending line number (1-indexed)."),
});

export const FileReadTool: ITool<FileReadInput, FileReadOutput> = {
  metadata: {
    name: "file_read",
    description:
      "Read a file's content. Supports optional line range (1-indexed). If no range is provided, the entire file is read.",
    scope: ToolScope.FilesystemRead,
  },
  input: FileReadInputSchema,
  execute: async (
    input: FileReadInput,
    context: ToolContext,
  ): Promise<ToolResult<FileReadOutput>> => {
    try {
      const filePath = validatePath(input.file, context);

      if (!(await context.host.exists(filePath))) {
        return { success: false, summary: `File ${input.file} does not exist.`, error: `File ${input.file} does not exist.` };
      }

      const content = await context.host.readFile(filePath);

      if (isBinaryFile(filePath, content)) {
        return {
          success: false,
          summary: `Cannot read binary file: ${input.file}.`,
          error: `Cannot read binary file: ${input.file}.`,
        };
      }

      const lines = content.split("\n");
      const totalLines = lines.length;

      let start = input.startLine !== undefined ? input.startLine - 1 : 0;
      let end = input.endLine !== undefined ? input.endLine : totalLines;

      // Bounds checking
      start = Math.max(0, Math.min(start, totalLines));
      end = Math.max(start, Math.min(end, totalLines));

      const slicedContent = lines.slice(start, end).join("\n");

      return {
        success: true,
        summary: `Read ${lines.slice(start, end).length} lines from ${input.file}.`,
        output: {
          content: slicedContent,
          totalLines,
        },
      };
    } catch (e: any) {
      return { success: false, summary: `Failed to read ${input.file}`, error: e.message };
    }
  },
  summarizeInput: (input: FileReadInput) => {
    let range = "";
    if (input.startLine !== undefined || input.endLine !== undefined) {
      range = ` [L${input.startLine || 1}-${input.endLine || "end"}]`;
    }
    return `"${input.file}"${range}`;
  },
  summary: (output: ToolResult<FileReadOutput>) => {
    return output.summary;
  },
};

// =============================================================================
// STRING SIMILARITY (for fuzzy matching)
// =============================================================================

function levenshteinDistance(a: string, b: string): number {
  const matrix: number[][] = [];

  for (let i = 0; i <= b.length; i++) {
    matrix[i] = [i];
  }
  for (let j = 0; j <= a.length; j++) {
    matrix[0]![j] = j;
  }

  for (let i = 1; i <= b.length; i++) {
    for (let j = 1; j <= a.length; j++) {
      if (b.charAt(i - 1) === a.charAt(j - 1)) {
        matrix[i]![j] = matrix[i - 1]![j - 1]!;
      } else {
        matrix[i]![j] = Math.min(
          matrix[i - 1]![j - 1]! + 1,
          matrix[i]![j - 1]! + 1,
          matrix[i - 1]![j]! + 1,
        );
      }
    }
  }

  return matrix[b.length]![a.length]!;
}

function similarity(a: string, b: string): number {
  if (a === b) return 1;
  if (a.length === 0 || b.length === 0) return 0;
  const distance = levenshteinDistance(a, b);
  return 1 - distance / Math.max(a.length, b.length);
}

function findFuzzyMatch(
  content: string,
  search: string,
  threshold: number = 0.95,
): { index: number; matched: string; similarity: number } | null {
  if (search.length === 0) return null;

  // First try exact match
  const exactIndex = content.indexOf(search);
  if (exactIndex !== -1) {
    return { index: exactIndex, matched: search, similarity: 1 };
  }

  // Sliding window for fuzzy match
  let bestMatch: { index: number; matched: string; similarity: number } | null =
    null;
  const searchLen = search.length;
  const windowSizes = [
    searchLen,
    Math.floor(searchLen * 1.1),
    Math.floor(searchLen * 0.9),
  ];

  for (const windowSize of windowSizes) {
    if (windowSize <= 0 || windowSize > content.length) continue;

    for (let i = 0; i <= content.length - windowSize; i++) {
      const candidate = content.substring(i, i + windowSize);
      const sim = similarity(search, candidate);

      if (sim >= threshold && (!bestMatch || sim > bestMatch.similarity)) {
        bestMatch = { index: i, matched: candidate, similarity: sim };
        if (sim > 0.98) break;
      }
    }
    if (bestMatch && bestMatch.similarity > 0.97) break;
  }

  return bestMatch;
}

function findLineBasedMatch(
  content: string,
  searchLines: string[],
  threshold: number = 0.95,
): {
  startLine: number;
  endLine: number;
  matchedLines: string[];
  similarity: number;
} | null {
  const contentLines = content.split("\n");

  if (searchLines.length === 0) return null;

  // Try exact line match first
  const searchJoined = searchLines.join("\n");
  const exactIndex = content.indexOf(searchJoined);
  if (exactIndex !== -1) {
    const startLine = content.substring(0, exactIndex).split("\n").length - 1;
    return {
      startLine,
      endLine: startLine + searchLines.length - 1,
      matchedLines: searchLines,
      similarity: 1,
    };
  }

  // Sliding window for fuzzy line match
  let bestMatch: {
    startLine: number;
    endLine: number;
    matchedLines: string[];
    similarity: number;
  } | null = null;

  for (
    let start = 0;
    start <= contentLines.length - searchLines.length;
    start++
  ) {
    const candidateLines = contentLines.slice(
      start,
      start + searchLines.length,
    );
    const candidate = candidateLines.join("\n");
    const sim = similarity(searchJoined, candidate);

    if (sim >= threshold && (!bestMatch || sim > bestMatch.similarity)) {
      bestMatch = {
        startLine: start,
        endLine: start + searchLines.length - 1,
        matchedLines: candidateLines,
        similarity: sim,
      };
    }
  }

  // Also try with +-1 line tolerance
  for (const delta of [1, -1, 2, -2]) {
    const windowSize = searchLines.length + delta;
    if (windowSize <= 0 || windowSize > contentLines.length) continue;

    for (let start = 0; start <= contentLines.length - windowSize; start++) {
      const candidateLines = contentLines.slice(start, start + windowSize);
      const candidate = candidateLines.join("\n");
      const sim = similarity(searchJoined, candidate);

      if (sim >= threshold && (!bestMatch || sim > bestMatch.similarity)) {
        bestMatch = {
          startLine: start,
          endLine: start + windowSize - 1,
          matchedLines: candidateLines,
          similarity: sim,
        };
      }
    }
  }

  return bestMatch;
}

// =============================================================================
// UNIFIED DIFF PARSER (git-style patches)
// =============================================================================

interface DiffHunk {
  oldStart: number;
  oldLines: number;
  newStart: number;
  newLines: number;
  lines: string[]; // Lines with ' ', '+', '-' prefix
}

interface ParsedDiff {
  oldFile: string;
  newFile: string;
  hunks: DiffHunk[];
}

function parseUnifiedDiff(diff: string): ParsedDiff | null {
  const lines = diff.split("\n");
  let oldFile = "";
  let newFile = "";
  const hunks: DiffHunk[] = [];

  let i = 0;

  // Parse header
  while (i < lines.length) {
    const line = lines[i]!;

    if (line.startsWith("--- ")) {
      oldFile = line.substring(4).replace(/^a\//, "");
    } else if (line.startsWith("+++ ")) {
      newFile = line.substring(4).replace(/^b\//, "");
    } else if (line.startsWith("@@")) {
      break;
    }
    i++;
  }

  // Parse hunks
  const hunkHeaderRegex = /^@@\s+-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s+@@/;

  while (i < lines.length) {
    const line = lines[i]!;
    const match = line.match(hunkHeaderRegex);

    if (match) {
      const hunk: DiffHunk = {
        oldStart: parseInt(match[1]!, 10),
        oldLines: match[2] ? parseInt(match[2], 10) : 1,
        newStart: match[3] ? parseInt(match[3], 10) : 1,
        newLines: match[4] ? parseInt(match[4], 10) : 1,
        lines: [],
      };

      i++;

      // Collect hunk lines
      while (i < lines.length) {
        const hunkLine = lines[i]!;

        if (
          hunkLine.startsWith("@@") ||
          hunkLine.startsWith("diff ") ||
          hunkLine.startsWith("--- ") ||
          hunkLine.startsWith("+++ ")
        ) {
          break;
        }

        if (
          hunkLine.startsWith(" ") ||
          hunkLine.startsWith("+") ||
          hunkLine.startsWith("-") ||
          hunkLine === ""
        ) {
          hunk.lines.push(hunkLine);
        }

        i++;
      }

      hunks.push(hunk);
    } else {
      i++;
    }
  }

  if (hunks.length === 0) {
    return null;
  }

  return { oldFile, newFile, hunks };
}

function applyDiffHunks(
  content: string,
  hunks: DiffHunk[],
  threshold: number = 0.95,
): { content: string; appliedHunks: number; errors: string[] } {
  const lines = content.split("\n");
  const errors: string[] = [];
  let appliedHunks = 0;

  // Sort hunks by oldStart descending (apply from bottom up)
  const sortedHunks = [...hunks].sort((a, b) => b.oldStart - a.oldStart);

  for (const hunk of sortedHunks) {
    const startIdx = hunk.oldStart - 1; // Convert to 0-indexed

    // Extract context lines from hunk (lines starting with ' ' or '-')
    const contextLines: string[] = [];
    const newLines: string[] = [];

    for (const hunkLine of hunk.lines) {
      if (hunkLine.startsWith(" ")) {
        contextLines.push(hunkLine.substring(1));
        newLines.push(hunkLine.substring(1));
      } else if (hunkLine.startsWith("-")) {
        contextLines.push(hunkLine.substring(1));
      } else if (hunkLine.startsWith("+")) {
        newLines.push(hunkLine.substring(1));
      } else if (hunkLine === "") {
        // Empty line could be a context line
        contextLines.push("");
        newLines.push("");
      }
    }

    // Try to find matching context in the file
    let matchStartIdx = -1;
    let bestSimilarity = 0;

    // First try exact match at expected position
    const expectedEndIdx = Math.min(
      startIdx + contextLines.length,
      lines.length,
    );
    const expectedSlice = lines.slice(startIdx, expectedEndIdx);
    const expectedContent = expectedSlice.join("\n");
    const contextContent = contextLines.join("\n");

    if (expectedContent === contextContent) {
      matchStartIdx = startIdx;
      bestSimilarity = 1;
    } else {
      // Fuzzy search for context
      for (
        let searchIdx = Math.max(0, startIdx - 10);
        searchIdx <=
        Math.min(lines.length - contextLines.length, startIdx + 10);
        searchIdx++
      ) {
        const candidate = lines
          .slice(searchIdx, searchIdx + contextLines.length)
          .join("\n");
        const sim = similarity(contextContent, candidate);

        if (sim >= threshold && sim > bestSimilarity) {
          bestSimilarity = sim;
          matchStartIdx = searchIdx;
        }
      }

      // Also try different context lengths
      for (const delta of [1, -1, 2, -2]) {
        const ctxLen = contextLines.length + delta;
        if (ctxLen <= 0 || ctxLen > lines.length) continue;

        for (
          let searchIdx = Math.max(0, startIdx - 10);
          searchIdx <= Math.min(lines.length - ctxLen, startIdx + 10);
          searchIdx++
        ) {
          const candidate = lines
            .slice(searchIdx, searchIdx + ctxLen)
            .join("\n");
          const sim = similarity(contextContent, candidate);

          if (sim >= threshold && sim > bestSimilarity) {
            bestSimilarity = sim;
            matchStartIdx = searchIdx;
          }
        }
      }
    }

    if (matchStartIdx === -1) {
      errors.push(
        `Hunk at line ${hunk.oldStart}: No matching context found (${(threshold * 100).toFixed(0)}% threshold)`,
      );
      continue;
    }

    // Apply the hunk
    const deleteCount = hunk.oldLines;
    lines.splice(matchStartIdx, deleteCount, ...newLines);
    appliedHunks++;
  }

  return { content: lines.join("\n"), appliedHunks, errors };
}

// =============================================================================
// FILE EDIT TOOL (write + replace + patch + apply_diff)
// =============================================================================

const FileEditOperationSchema = z.union([
  z.literal("write"),
  z.literal("replace"),
  z.literal("patch"),
  z.literal("apply_diff"),
]);

type FileEditOperation = z.infer<typeof FileEditOperationSchema>;

export interface PatchHunk {
  startLine: number;
  endLine: number;
  action: "replace" | "insert" | "delete";
  content?: string;
}

export interface FileEditInput {
  operation: FileEditOperation;
  file: string;
  content?: string;
  search?: string;
  replace?: string;
  threshold?: number;
  hunks?: PatchHunk[];
  exact?: boolean;
  diff?: string;
}

export interface FileEditOutput {
  matches?: number;
  matchedContent?: string;
  similarity?: number;
  appliedHunks?: number;
  diagnostics?: LSPDiagnostic[];
}

const PatchHunkSchema = z.object({
  startLine: z.number().describe("1-indexed start line number"),
  endLine: z.number().describe("1-indexed end line number (inclusive)"),
  action: z.enum(["replace", "insert", "delete"]).describe("Action to perform"),
  content: z
    .string()
    .optional()
    .describe("Content to insert/replace with (not needed for delete)"),
});

const FileEditInputSchema = z
  .object({
    operation: FileEditOperationSchema.describe(
      "Operation: 'write' to overwrite, 'replace' for search/replace (fuzzy by default), 'patch' for line-based edits, 'apply_diff' for git-style unified diffs.",
    ),
    file: z.string().describe("The path of the file to edit."),
    content: z
      .string()
      .optional()
      .describe("Full content to write (required for 'write' operation)."),
    search: z
      .string()
      .optional()
      .describe(
        "String to search for (required for 'replace'). Uses fuzzy matching by default (95% threshold).",
      ),
    replace: z
      .string()
      .optional()
      .describe("Replacement string (required for 'replace')."),
    threshold: z
      .number()
      .min(0.5)
      .max(1)
      .optional()
      .default(0.95)
      .describe(
        "Similarity threshold for fuzzy matching (0.5-1.0, default 0.95). Only used if exact=false.",
      ),
    exact: z
      .boolean()
      .optional()
      .default(false)
      .describe(
        "If true, requires exact match instead of fuzzy. Default: false (fuzzy matching enabled).",
      ),
    hunks: z
      .array(PatchHunkSchema)
      .optional()
      .describe(
        "Array of patch hunks for line-based editing (required for 'patch'). Hunks are applied in order.",
      ),
    diff: z
      .string()
      .optional()
      .describe(
        "Git-style unified diff to apply (required for 'apply_diff'). Supports standard diff format with ---, +++, @@ headers.",
      ),
  })
  .refine(
    (data) => {
      if (data.operation === "write") {
        return data.content !== undefined;
      }
      if (data.operation === "replace") {
        return data.search !== undefined && data.replace !== undefined;
      }
      if (data.operation === "patch") {
        return data.hunks !== undefined && data.hunks.length > 0;
      }
      if (data.operation === "apply_diff") {
        return data.diff !== undefined && data.diff.length > 0;
      }
      return false;
    },
    { message: "Invalid parameters for operation." },
  );

export const FileEditTool: ITool<FileEditInput, FileEditOutput> = {
  metadata: {
    name: "file_edit",
    description: `Edit a file with multiple strategies:
- 'write': Overwrite entire file or create new file
- 'replace': Search and replace with fuzzy matching by default (95% similarity). Set exact=true for strict matching.
- 'patch': Line-based edits with hunks (startLine, endLine, action, content)
- 'apply_diff': Apply git-style unified diff patches. Pass the diff string with ---/+++/@@ headers.

**CRITICAL**: File must be fully allocated (watched without limit) before editing.
Replace uses fuzzy matching by default - it will find the closest match even if the search string has minor differences from the file content (whitespace, comments, etc.).
apply_diff is useful when you have a diff from git or want to make precise line-based changes using standard diff format.`,
    scope: ToolScope.FilesystemWrite,
  },
  input: FileEditInputSchema,
  execute: async (
    input: FileEditInput,
    context: ToolContext,
  ): Promise<ToolResult<FileEditOutput>> => {
    try {
      const filePath = validatePath(input.file, context, "write");
      const fileExists = await context.host.exists(filePath);

      // Capture before content for snapshot
      let beforeContent: string | null = null;
      if (fileExists) {
        try {
          beforeContent = await context.host.readFile(filePath);
        } catch {
          // Failed to read, will be null
        }
      }

      let newContent = "";
      let matchedContent: string | undefined;
      let matches: number | undefined;
      let similarityScore: number | undefined;
      let appliedHunks: number | undefined;
      let summary = "";

      // WRITE OPERATION
      if (input.operation === "write") {
        newContent = input.content!;
        summary = `Wrote ${newContent.length} bytes to ${input.file}.`;
      }
      // REPLACE OPERATION (fuzzy by default, exact if specified)
      else if (input.operation === "replace") {
        if (!beforeContent) {
           return { success: false, summary: `File ${input.file} does not exist.`, error: `File ${input.file} does not exist.` };
        }
        const exact = input.exact ?? false;
        const threshold = input.threshold ?? 0.95;

        // Try exact match first
        const exactMatchCount = beforeContent.split(input.search!).length - 1;

        if (exactMatchCount > 0) {
          newContent = beforeContent.replaceAll(input.search!, input.replace!);
          matches = exactMatchCount;
          similarityScore = 1;
          summary = `Exact match: Replaced ${exactMatchCount} occurrence(s) in ${input.file}.`;
        } else {
          if (exact) {
            return {
              success: false,
              summary: "No exact matches found.",
              error: "No exact matches found (exact mode enabled).",
            };
          }

          // Fuzzy matching
          const searchLines = input.search!.split("\n");
          if (searchLines.length > 1) {
            const match = findLineBasedMatch(beforeContent, searchLines, threshold);
            if (!match) {
              return {
                success: false,
                summary: "No fuzzy match found.",
                error: `No match found with ${(threshold * 100).toFixed(0)}% similarity.`,
              };
            }
            const contentLines = beforeContent.split("\n");
            newContent = [...contentLines.slice(0, match.startLine), ...input.replace!.split("\n"), ...contentLines.slice(match.endLine + 1)].join("\n");
            matchedContent = match.matchedLines.join("\n");
            similarityScore = match.similarity;
          } else {
            const fuzzyMatch = findFuzzyMatch(beforeContent, input.search!, threshold);
            if (!fuzzyMatch) {
              return {
                success: false,
                summary: "No fuzzy match found.",
                error: `No match found with ${(threshold * 100).toFixed(0)}% similarity.`,
              };
            }
            newContent = beforeContent.substring(0, fuzzyMatch.index) + input.replace! + beforeContent.substring(fuzzyMatch.index + fuzzyMatch.matched.length);
            matchedContent = fuzzyMatch.matched;
            similarityScore = fuzzyMatch.similarity;
          }
          matches = 1;
          summary = `Fuzzy match (${(similarityScore * 100).toFixed(1)}%) replaced in ${input.file}.`;
        }
      }
      // PATCH OPERATION
      else if (input.operation === "patch") {
        if (!beforeContent) return { success: false, summary: "File not found", error: "File not found" };
        const lines = beforeContent.split("\n");
        const sortedHunks = [...input.hunks!].sort((a, b) => b.startLine - a.startLine);
        appliedHunks = 0;
        for (const hunk of sortedHunks) {
          const startIdx = hunk.startLine - 1;
          const endIdx = hunk.endLine - 1;
          if (startIdx < 0 || endIdx >= lines.length || startIdx > endIdx) {
            return { success: false, summary: "Patch failed", error: `Invalid hunk: ${hunk.startLine}-${hunk.endLine} out of bounds.` };
          }
          if (hunk.action === "delete") lines.splice(startIdx, endIdx - startIdx + 1);
          else if (hunk.action === "insert") lines.splice(startIdx, 0, ...hunk.content!.split("\n"));
          else if (hunk.action === "replace") lines.splice(startIdx, endIdx - startIdx + 1, ...hunk.content!.split("\n"));
          appliedHunks++;
        }
        newContent = lines.join("\n");
        summary = `Applied ${appliedHunks} hunk(s) to ${input.file}.`;
      }
      // APPLY_DIFF OPERATION
      else if (input.operation === "apply_diff") {
        if (!beforeContent) return { success: false, summary: "File not found", error: "File not found" };
        const parsedDiff = parseUnifiedDiff(input.diff!);
        if (!parsedDiff) return { success: false, summary: "Failed to parse diff", error: "Failed to parse unified diff." };
        const res = applyDiffHunks(beforeContent, parsedDiff.hunks, input.threshold ?? 0.95);
        if (res.appliedHunks === 0) return { success: false, summary: "Failed to apply diff", error: res.errors.join("; ") };
        newContent = res.content;
        appliedHunks = res.appliedHunks;
        summary = `Applied ${appliedHunks}/${parsedDiff.hunks.length} diff hunk(s) to ${input.file}.`;
      }

      // Write the new content
      const success = await context.host.writeFile(filePath, newContent);
      if (!success) {
        return { success: false, summary: "Write failed", error: "Failed to write file." };
      }

      // LSP Diagnostics
      let diagnostics: LSPDiagnostic[] | undefined;
      const lsp = Engine.getLSPUtility(context.host, context.agent.environment.cwd.toString());
      const availability = await lsp.isAvailableForFile(filePath);
      if (availability.available) {
        await lsp.notifyFileChanged(filePath, newContent);
        diagnostics = await lsp.waitForDiagnostics(filePath, 3000);
      }

      const output: FileEditOutput = {
        matches,
        matchedContent: matchedContent ? (matchedContent.substring(0, 200) + (matchedContent.length > 200 ? "..." : "")) : undefined,
        similarity: similarityScore,
        appliedHunks,
        diagnostics,
      };

      await storeEditSnapshot(context, input.file, input.operation, beforeContent, newContent, diagnostics, input.hunks);

      return {
        success: true,
        summary,
        output,
      };
    } catch (error: any) {
      return {
        success: false,
        summary: `Edit failed: ${input.file}`,
        error: error.message,
      };
    }
  },
  summarizeInput: (input: FileEditInput) => {
    return `"${input.file}" [${input.operation}]`;
  },
  summary: (output: ToolResult<FileEditOutput>): string => {
    return output.summary;
  },
};

// =============================================================================
// FILE QUERY TOOL (list_dir + exists + search_dir)
// =============================================================================

const FileQueryOperationSchema = z.union([
  z.literal("list"),
  z.literal("exists"),
  z.literal("search"),
]);

type FileQueryOperation = z.infer<typeof FileQueryOperationSchema>;

export interface FileQueryEntry {
  name: string;
  isDirectory: boolean;
  size: number;
  mtime: number;
  owner?: string;
  group?: string;
  mode?: string;
}

export interface FileQueryMatch {
  file: string;
  line: number;
  content: string;
}

export interface FileQueryInput {
  operation: FileQueryOperation;
  path: string;
  exclude?: string[];
  query?: string;
  includes?: string[];
  excludes?: string[];
  contextLines?: number;
}

export interface FileQueryOutput {
  exists?: boolean;
  entries?: FileQueryEntry[];
  matches?: FileQueryMatch[];
  count?: number;
}

const FileQueryInputSchema = z
  .object({
    operation: FileQueryOperationSchema.describe(
      "Query operation: 'list' directory contents, 'exists' check path, or 'search' for text across files.",
    ),
    path: z
      .string()
      .describe(
        "The path to query (directory for 'list'/'search', any path for 'exists').",
      ),
    exclude: z
      .array(z.string())
      .optional()
      .describe(
        "Array of filenames/folders to exclude. Only used for 'list' operation.",
      ),
    query: z
      .string()
      .optional()
      .describe(
        "The text or regex to search for. Required for 'search' operation.",
      ),
    includes: z
      .array(z.string())
      .optional()
      .describe(
        "File patterns to include (e.g. ['*.ts', '*.js']). Only used for 'search' operation.",
      ),
    excludes: z
      .array(z.string())
      .optional()
      .describe(
        "File or directory patterns to exclude. Only used for 'search' operation.",
      ),
    contextLines: z
      .number()
      .optional()
      .describe(
        "Number of context lines before and after each match (equivalent to grep -B/-A). Only used for 'search' operation.",
      ),
  })
  .refine(
    (data) => {
      if (data.operation === "search") {
        return data.query !== undefined && data.query.length > 0;
      }
      return true;
    },
    { message: "'query' is required for 'search' operation." },
  );

export const FileQueryTool: ITool<FileQueryInput, FileQueryOutput> = {
  metadata: {
    name: "file_query",
    description:
      "Query file system: list directory contents, check path existence, or search for text patterns across files.",
    scope: ToolScope.FilesystemRead,
  },
  input: FileQueryInputSchema,
  execute: async (
    input: FileQueryInput,
    context: ToolContext,
  ): Promise<ToolResult<FileQueryOutput>> => {
    try {
      const queryPath = validatePath(input.path, context);

      if (input.operation === "exists") {
        const exists = await context.host.exists(queryPath);
        return { success: true, summary: `Path ${input.path} ${exists ? 'exists' : 'does not exist'}.`, output: { exists } };
      }

      if (input.operation === "list") {
        const entries = await context.host.listDir(queryPath, {
          exclude: input.exclude,
        });
        const outputEntries = entries.map((e) => ({
          name: e.name,
          isDirectory: e.isDirectory,
          size: e.size,
          mtime: e.mtime,
          owner: e.owner,
          group: e.group,
          mode: e.mode.toString(8),
        }));
        return {
          success: true,
          summary: `Listed ${outputEntries.length} items in ${input.path}.`,
          output: { entries: outputEntries },
        };
      }

      // search operation
      const sanitizedQuery = input.query!.replace(/"/g, '\\"');
      let cmd = `grep -E -rn "${sanitizedQuery}" "${queryPath}"`;
      if (input.contextLines && input.contextLines > 0) {
        cmd += ` -B ${input.contextLines} -A ${input.contextLines}`;
      }
      if (input.includes && input.includes.length > 0) {
        input.includes.forEach(p => { cmd += ` --include="${p}"`; });
      }
      const defaultExcludes = ["node_modules", ".git", ".next", "dist", "build"];
      const excludes = input.excludes ? [...input.excludes, ...defaultExcludes] : defaultExcludes;
      excludes.forEach(p => { cmd += ` --exclude-dir="${p}" --exclude="${p}"`; });

      const result = await context.host.exec(cmd, { cwd: context.agent.environment.cwd.toString() });
      if (result.exitCode !== 0 && result.exitCode !== 1) {
        return { success: false, summary: "Search failed", error: result.stderr || result.stdout };
      }

      const outputLines = (result.stdout || "").trim().split("\n").filter(l => l.length > 0);
      const matches: FileQueryMatch[] = outputLines.map(line => {
        const parts = line.split(":");
        if (parts.length < 3) return null;
        return { file: parts[0]!.trim(), line: parseInt(parts[1]!, 10), content: parts.slice(2).join(":").trim() };
      }).filter((m): m is FileQueryMatch => m !== null);

      return {
        success: true,
        summary: `Found ${matches.length} matches for "${input.query}" in ${input.path}.`,
        output: { matches, count: matches.length },
      };
    } catch (err: any) {
      return { success: false, summary: "Query failed", error: err.message };
    }
  },
  summarizeInput: (input: FileQueryInput) => {
    return `"${input.path}" [${input.operation}]`;
  },
  summary: (output: ToolResult<FileQueryOutput>) => {
    return output.summary;
  },
};

// =============================================================================
// FILE MANAGE TOOL (mkdir, delete, rename)
// =============================================================================

const FileManageOperationSchema = z.union([
  z.literal("mkdir"),
  z.literal("delete"),
  z.literal("rename"),
]);

type FileManageOperation = z.infer<typeof FileManageOperationSchema>;

export interface FileManageInput {
  operation: FileManageOperation;
  path: string;
  newPath?: string;
}

export interface FileManageOutput {
  message: string;
}

const FileManageInputSchema = z.object({
  operation: FileManageOperationSchema.describe(
    "Operation to perform: 'mkdir', 'delete', or 'rename'.",
  ),
  path: z.string().describe("Path of the file or directory."),
  newPath: z.string().optional().describe("New path (required for rename)."),
});

export const FileManageTool: ITool<FileManageInput, FileManageOutput> = {
  metadata: {
    name: "file_manage",
    description: "Manage files and directories (mkdir, delete, rename).",
    scope: ToolScope.FilesystemWrite,
  },
  input: FileManageInputSchema,
  execute: async (
    input: FileManageInput,
    context: ToolContext,
  ): Promise<ToolResult<FileManageOutput>> => {
    try {
      const filePath = validatePath(input.path, context, "write");

      if (input.operation === "mkdir") {
        if (await context.host.exists(filePath)) return { success: false, summary: "Already exists", error: "Directory already exists." };
        const success = await context.host.mkdir(filePath);
        return { success, summary: success ? `Created directory ${input.path}` : "Failed to create directory", output: { message: success ? "Success" : "Failed" } };
      }

      if (input.operation === "delete") {
        if (!(await context.host.exists(filePath))) return { success: false, summary: "Not found", error: "File or directory not found." };
        const success = await context.host.remove(filePath);
        return { success, summary: success ? `Deleted ${input.path}` : "Failed to delete", output: { message: success ? "Success" : "Failed" } };
      }

      if (input.operation === "rename") {
        if (!input.newPath) return { success: false, summary: "Missing newPath", error: "newPath is required for rename." };
        if (!(await context.host.exists(filePath))) return { success: false, summary: "Not found", error: "Source not found." };
        const newFilePath = validatePath(input.newPath, context, "write");
        if (await context.host.exists(newFilePath)) return { success: false, summary: "Destination exists", error: "Destination already exists." };

        // For simplicity in this tool, we read and write for files. For directories we list and move.
        // Reusing the logic from the previous version.
        const stat = await context.host.stat(filePath);
        if (stat.isDirectory) {
           await context.host.mkdir(newFilePath);
           const moveSuccess = await moveDirectory(filePath, newFilePath, context);
           if (!moveSuccess) return { success: false, summary: "Rename failed", error: "Failed to move directory contents." };
           await context.host.remove(filePath);
        } else {
           const content = await context.host.readFile(filePath);
           await context.host.writeFile(newFilePath, content);
           await context.host.remove(filePath);
        }
        return { success: true, summary: `Renamed ${input.path} to ${input.newPath}`, output: { message: "Success" } };
      }

      return { success: false, summary: "Unknown operation", error: "Unknown operation" };
    } catch (error: any) {
      return { success: false, summary: "Operation failed", error: error.message };
    }
  },
  summarizeInput: (input: FileManageInput) => {
    return `${input.operation}: ${input.path}`;
  },
  summary: (output: ToolResult<FileManageOutput>) => {
    return output.summary;
  },
};

async function moveDirectory(oldPath: string, newPath: string, context: ToolContext): Promise<boolean> {
  const entries = await context.host.listDir(oldPath);
  for (const entry of entries) {
    const oldEntry = resolve(oldPath, entry.name);
    const newEntry = resolve(newPath, entry.name);
    if (entry.isDirectory) {
      await context.host.mkdir(newEntry);
      if (!(await moveDirectory(oldEntry, newEntry, context))) return false;
    } else {
      const content = await context.host.readFile(oldEntry);
      if (!(await context.host.writeFile(newEntry, content))) return false;
    }
  }
  return true;
}

export const AllFileTools = [
  FileReadTool,
  FileEditTool,
  FileQueryTool,
  FileManageTool,
];

// Compatibility
export const WriteFileTool = FileEditTool;
