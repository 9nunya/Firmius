'use client';

import { useEffect, useCallback, useState } from 'react';
import { useParams } from 'next/navigation';
import useAppStore from '../stores/app-store';
import { client } from '@firmius/shared/api';
import type { Thread } from '@firmius/shared/api';

export function useThread() {
  const params = useParams();
  const { activeThreadId, selectThread, threads, loadThreads } = useAppStore();
  const [loadError, setLoadError] = useState<string | null>(null);

  const threadId = params?.id as string | undefined;

  const loadThread = useCallback(async (id: string) => {
    setLoadError(null);
    try {
      const thread = await client.getThread(id);
      if (thread) {
        await selectThread(id);
      } else {
        setLoadError('Thread not found');
      }
    } catch (error) {
      console.error('Failed to load thread:', error);
      setLoadError('Failed to load thread');
    }
  }, [selectThread]);

  useEffect(() => {
    if (threadId && threadId !== activeThreadId) {
      loadThread(threadId);
    }
  }, [threadId, activeThreadId, loadThread]);

  useEffect(() => {
    loadThreads();
  }, [loadThreads]);

  const currentThread = threads.find((t: Thread) => t.id === activeThreadId);

  return {
    threadId,
    activeThreadId,
    currentThread,
    selectThread,
    loadError
  };
}
