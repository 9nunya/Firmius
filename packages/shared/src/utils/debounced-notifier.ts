export class DebouncedNotifier {
  private timeoutId: ReturnType<typeof setTimeout> | null = null;
  private readonly delay: number;
  private callback: ((files: string[]) => void) | null = null;
  private pendingFiles: Set<string> = new Set();

  constructor(delay: number = 300) {
    this.delay = delay;
  }

  onChange(callback: (files: string[]) => void): void {
    this.callback = callback;
  }

  notify(filePath: string): void {
    this.pendingFiles.add(filePath);

    if (this.timeoutId) {
      clearTimeout(this.timeoutId);
    }

    this.timeoutId = setTimeout(() => {
      this.flush();
    }, this.delay);
  }

  flush(): void {
    if (this.timeoutId) {
      clearTimeout(this.timeoutId);
      this.timeoutId = null;
    }

    if (this.pendingFiles.size > 0 && this.callback) {
      const files = Array.from(this.pendingFiles);
      this.pendingFiles.clear();
      this.callback(files);
    }
  }

  clear(): void {
    if (this.timeoutId) {
      clearTimeout(this.timeoutId);
      this.timeoutId = null;
    }
    this.pendingFiles.clear();
  }

  dispose(): void {
    this.clear();
    this.callback = null;
  }
}
