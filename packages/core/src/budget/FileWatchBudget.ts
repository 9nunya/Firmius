import type { ProviderMessage } from "@firmius/shared/types";
import { type FileWatchEntry } from "./Types";

export class FileWatchBudget {
  private files: Map<string, FileWatchEntry> = new Map();
  private maxChars: number;

  constructor(maxChars: number = 50000) {
    this.maxChars = maxChars;
  }

  setMaxChars(chars: number): void {
    this.maxChars = chars;
  }

  watch(path: string, content: string, offset: number = 0, limit?: number): boolean {
    const lines = content.split('\n');
    const effectiveLimit = limit ?? lines.length;
    const end = Math.min(offset + effectiveLimit, lines.length);
    const slicedContent = lines.slice(offset, end).join('\n');
    const charCount = slicedContent.length;

    const existing = this.files.get(path);
    if (existing) {
      this.maxChars += existing.charCount;
    }

    if (charCount > this.maxChars) {
      if (existing) {
        this.maxChars -= existing.charCount;
      }
      return false;
    }

    const entry: FileWatchEntry = {
      path,
      offset,
      limit: effectiveLimit,
      content: slicedContent,
      charCount,
      mtime: Date.now(),
      isFullFile: offset === 0 && end === lines.length,
    };

    if (existing) {
      this.maxChars -= charCount;
    } else {
      this.maxChars -= charCount;
    }

    this.files.set(path, entry);
    return true;
  }

  unwatch(path: string): FileWatchEntry | null {
    const entry = this.files.get(path);
    if (entry) {
      this.maxChars += entry.charCount;
      this.files.delete(path);
      return entry;
    }
    return null;
  }

  updateContent(path: string, content: string): boolean {
    const existing = this.files.get(path);
    if (!existing) return false;

    const lines = content.split('\n');
    const end = Math.min(existing.offset + existing.limit, lines.length);
    const slicedContent = lines.slice(existing.offset, end).join('\n');
    const newCharCount = slicedContent.length;

    const charDiff = newCharCount - existing.charCount;
    if (this.maxChars + charDiff < 0) {
      return false;
    }

    this.maxChars += existing.charCount - newCharCount;
    existing.content = slicedContent;
    existing.charCount = newCharCount;
    existing.mtime = Date.now();
    existing.isFullFile = existing.offset === 0 && end === lines.length;

    return true;
  }

  canWatch(charCount: number): boolean {
    return charCount <= this.maxChars;
  }

  getEntry(path: string): FileWatchEntry | null {
    return this.files.get(path) ?? null;
  }

  hasFile(path: string): boolean {
    return this.files.has(path);
  }

  isFullFile(path: string): boolean {
    const entry = this.files.get(path);
    return entry?.isFullFile ?? false;
  }

  getTokenUsage(): number {
    return Math.ceil(this.getCharCount() / 4);
  }

  getCharCount(): number {
    let total = 0;
    for (const entry of this.files.values()) {
      total += entry.charCount;
    }
    return total;
  }

  getFileCount(): number {
    return this.files.size;
  }

  getRemainingChars(): number {
    return this.maxChars;
  }

  getRemainingTokens(): number {
    return Math.ceil(this.maxChars / 4);
  }

  clear(): void {
    for (const entry of this.files.values()) {
      this.maxChars += entry.charCount;
    }
    this.files.clear();
  }

  getAllEntries(): FileWatchEntry[] {
    return Array.from(this.files.values());
  }

  getAllPaths(): string[] {
    return Array.from(this.files.keys());
  }

  toProviderMessages(): ProviderMessage[] {
    if (this.files.size === 0) return [];

    const messages: ProviderMessage[] = [];
    let fileContent = '\n' + '='.repeat(80) + '\n';
    fileContent += '!! IMPORTANT !! BELOW ARE YOUR WATCHED FILES\n';
    fileContent += 'Each file is separated by a clear delimiter. Lines are prefixed with line numbers.\n';
    fileContent += '='.repeat(80) + '\n\n';

    for (const entry of this.files.values()) {
      const lines = entry.content.split('\n');
      const formattedContent = lines
        .map((line, i) => `${(entry.offset + i + 1).toString().padStart(4, ' ')} | ${line}`)
        .join('\n');

      fileContent += '\n' + '█'.repeat(80) + '\n';
      fileContent += `█ FILE: ${entry.path}\n`;
      fileContent += `█ LINES: ${entry.offset + 1}-${entry.offset + lines.length} | STATUS: ${entry.isFullFile ? '✓ FULLY ALLOCATED (editable)' : '⚠ PARTIAL (read-only)'}\n`;
      fileContent += '█'.repeat(80) + '\n';
      fileContent += formattedContent + '\n';
      fileContent += '█'.repeat(80) + '\n';
      fileContent += `█ END OF ${entry.isFullFile ? 'FULL FILE' : 'PARTIAL VIEW'}\n`;
      fileContent += '█'.repeat(80) + '\n';
    }

    messages.push({
      role: 'user',
      content: fileContent,
    });

    return messages;
  }

  getState(): { files: Map<string, FileWatchEntry>; maxChars: number } {
    return {
      files: new Map(this.files),
      maxChars: this.maxChars,
    };
  }

  restoreState(state: { files: Map<string, FileWatchEntry>; maxChars: number }): void {
    this.files = new Map(state.files);
    this.maxChars = state.maxChars;
  }
}
