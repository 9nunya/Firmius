type WriteTask = () => Promise<void>;

export class WriteQueue {
  private queue: WriteTask[] = [];
  private isProcessing: boolean = false;
  private currentResolve: (() => void) | null = null;

  /**
   * Adds a task to the queue and returns a promise that resolves when the task completes.
   * Tasks are executed serially in the order they were added.
   *
   * @param task - The asynchronous task to execute
   * @returns A promise that resolves when the task completes
   */
  public async run(task: WriteTask): Promise<void> {
    return new Promise((resolve) => {
      this.queue.push(async () => {
        await task();
        resolve();
      });

      if (!this.isProcessing) {
        this.processQueue();
      }
    });
  }

  /**
   * Waits for all pending tasks in the queue to complete.
   * Returns immediately if the queue is empty or already processing nothing.
   *
   * @returns A promise that resolves when all pending tasks are complete
   */
  public async flush(): Promise<void> {
    if (this.queue.length === 0 && !this.isProcessing) {
      return Promise.resolve();
    }

    return new Promise((resolve) => {
      this.currentResolve = resolve;
    });
  }

  /**
   * Returns the number of pending tasks in the queue.
   *
   * @returns The count of tasks waiting to be processed
   */
  public getQueueLength(): number {
    return this.queue.length;
  }

  /**
   * Returns whether the queue is currently processing tasks.
   *
   * @returns True if the queue is processing, false otherwise
   */
  public isRunning(): boolean {
    return this.isProcessing;
  }

  private async processQueue(): Promise<void> {
    this.isProcessing = true;

    while (this.queue.length > 0) {
      const task = this.queue.shift();
      if (task) {
        await task();
      }
    }

    this.isProcessing = false;

    if (this.currentResolve) {
      this.currentResolve();
      this.currentResolve = null;
    }
  }
}
