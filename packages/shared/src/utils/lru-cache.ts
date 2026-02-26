interface LRUCachedItem<V> {
  key: string;
  value: V;
  next: LRUCachedItem<V> | null;
  prev: LRUCachedItem<V> | null;
}

export class LRUCache<V> {
  private cache: Map<string, LRUCachedItem<V>>;
  private head: LRUCachedItem<V> | null = null;
  private tail: LRUCachedItem<V> | null = null;
  private limit: number;
  private onEvict: ((key: string, value: V) => void) | null = null;

  constructor(limit: number = 15000) {
    this.cache = new Map();
    this.limit = limit;
  }

  setOnEvict(callback: (key: string, value: V) => void): void {
    this.onEvict = callback;
  }

  get(key: string): V | undefined {
    const item = this.cache.get(key);
    if (!item) return undefined;

    this.moveToFront(item);
    return item.value;
  }

  set(key: string, value: V): void {
    const existing = this.cache.get(key);
    if (existing) {
      existing.value = value;
      this.moveToFront(existing);
      return;
    }

    if (this.cache.size >= this.limit) {
      this.evictLRU();
    }

    const newItem: LRUCachedItem<V> = {
      key,
      value,
      next: null,
      prev: null,
    };

    this.cache.set(key, newItem);
    this.addToFront(newItem);
  }

  has(key: string): boolean {
    return this.cache.has(key);
  }

  clear(): void {
    this.cache.clear();
    this.head = null;
    this.tail = null;
  }

  keys(): IterableIterator<string> {
    return this.cache.keys();
  }

  values(): IterableIterator<V> {
    const values: V[] = [];
    for (const item of this.cache.values()) {
      values.push(item.value);
    }
    return values[Symbol.iterator]();
  }

  entries(): IterableIterator<[string, V]> {
    const entries: [string, V][] = [];
    for (const [key, item] of this.cache.entries()) {
      entries.push([key, item.value]);
    }
    return entries[Symbol.iterator]();
  }

  get size(): number {
    return this.cache.size;
  }

  private moveToFront(item: LRUCachedItem<V>): void {
    if (item === this.head) return;

    this.removeFromList(item);
    this.addToFront(item);
  }

  private addToFront(item: LRUCachedItem<V>): void {
    item.next = this.head;
    item.prev = null;

    if (this.head) {
      this.head.prev = item;
    }

    this.head = item;

    if (!this.tail) {
      this.tail = item;
    }
  }

  private removeFromList(item: LRUCachedItem<V>): void {
    if (item.prev) {
      item.prev.next = item.next;
    } else {
      this.head = item.next;
    }

    if (item.next) {
      item.next.prev = item.prev;
    } else {
      this.tail = item.prev;
    }
  }

  private evictLRU(): void {
    if (!this.tail) return;

    const item = this.tail;
    this.cache.delete(item.key);
    this.removeFromList(item);

    if (this.onEvict) {
      this.onEvict(item.key, item.value);
    }
  }
}
