import { watch, type FSWatcher } from "node:fs";
import EventEmitter from "node:events";
import { logger } from "@firmius/shared";

interface FileWatchEntry {
  path: string;
  watcher: FSWatcher;
  lastMtime: number;
}

export interface FileWatcherEvents {
  fileChanged: (path: string) => void;
  fileDeleted: (path: string) => void;
  error: (path: string, error: Error) => void;
}

export class FileWatcher extends EventEmitter {
  private watches: Map<string, FileWatchEntry> = new Map();
  private debounceTimers: Map<string, NodeJS.Timeout> = new Map();
  private readonly debounceMs: number = 100;

  watch(filePath: string): void {
    if (this.watches.has(filePath)) {
      return;
    }

    const maxRetries = 3;
    const retryDelays = [200, 500, 1000];

    const attemptWatch = (attempt: number): void => {
      try {
        const watcher = watch(filePath, (eventType) => {
          this.handleFileEvent(filePath, eventType);
        });

        this.watches.set(filePath, {
          path: filePath,
          watcher,
          lastMtime: Date.now(),
        });
      } catch (error) {
        const err = error as Error;

        if (err.message.includes('ENOENT')) {
          if (attempt < maxRetries - 1) {
            setTimeout(() => attemptWatch(attempt + 1), retryDelays[attempt]);
          } else {
            logger.debug(`[FileWatcher] File not yet available for watching: ${filePath}`);
          }
        } else {
          this.emit("error", filePath, err);
        }
      }
    };

    attemptWatch(0);
  }

  unwatch(filePath: string): void {
    const entry = this.watches.get(filePath);
    if (entry) {
      entry.watcher.close();
      this.watches.delete(filePath);
    }

    const timer = this.debounceTimers.get(filePath);
    if (timer) {
      clearTimeout(timer);
      this.debounceTimers.delete(filePath);
    }
  }

  unwatchAll(): void {
    for (const [, entry] of this.watches) {
      entry.watcher.close();
    }
    this.watches.clear();

    for (const [, timer] of this.debounceTimers) {
      clearTimeout(timer);
    }
    this.debounceTimers.clear();
  }

  private handleFileEvent(filePath: string, eventType: string): void {
    const existingTimer = this.debounceTimers.get(filePath);
    if (existingTimer) {
      clearTimeout(existingTimer);
    }

    const timer = setTimeout(() => {
      this.debounceTimers.delete(filePath);

      if (eventType === "rename") {
        this.emit("fileDeleted", filePath);
        this.unwatch(filePath);
      } else {
        this.emit("fileChanged", filePath);
      }
    }, this.debounceMs);

    this.debounceTimers.set(filePath, timer);
  }

  getWatchedFiles(): string[] {
    return Array.from(this.watches.keys());
  }

  isWatching(filePath: string): boolean {
    return this.watches.has(filePath);
  }
}
