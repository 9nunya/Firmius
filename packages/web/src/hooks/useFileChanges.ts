'use client';

import { useState, useEffect, useCallback, useMemo, useRef } from 'react';
import { client } from '@firmius/shared/api';

interface FileChange {
  file: string;
  operation: string;
  additions: number;
  deletions: number;
  timestamp: number;
}

interface ChangesData {
  additions: number;
  deletions: number;
  files: FileChange[];
  totalFiles: number;
}

interface UseFileChangesOptions {
  threadId: string | null;
  agentId: string | null;
  turnIndex?: number | null;
  enabled?: boolean;
}

export function useFileChanges({
  threadId,
  agentId,
  turnIndex,
  enabled = true,
}: UseFileChangesOptions) {
  const [data, setData] = useState<ChangesData | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<Error | null>(null);

  // Use refs to avoid dependency issues with intervals
  const stateRef = useRef({ threadId, agentId, turnIndex });
  stateRef.current = { threadId, agentId, turnIndex };

  const fetchChanges = useCallback(async () => {
    const { threadId: tId, agentId: aId, turnIndex: tIdx } = stateRef.current;
    
    if (!tId || !enabled) return;

    setIsLoading(true);
    setError(null);

    try {
      let response: ChangesData;

      if (aId) {
        // Get changes for specific agent
        const url = `/api/threads/${tId}/agents/${aId}/changes`;
        const params = tIdx !== undefined && tIdx !== null
          ? `?turnIndex=${tIdx}`
          : '';
        
        const res = await fetch(`${url}${params}`);
        if (res.status === 404) {
          // No changes yet - treat as empty state
          response = { additions: 0, deletions: 0, totalFiles: 0, files: [] };
        } else if (!res.ok) {
          throw new Error('Failed to fetch changes');
        } else {
          response = await res.json();
        }
      } else {
        // Get all thread changes (for lead agent view)
        const res = await fetch(`/api/threads/${tId}/changes`);
        if (res.status === 404) {
          // No changes yet - treat as empty state
          response = { additions: 0, deletions: 0, totalFiles: 0, files: [] };
        } else if (!res.ok) {
          throw new Error('Failed to fetch changes');
        } else {
          response = await res.json();
        }
      }

      setData(response);
    } catch (err) {
      setError(err instanceof Error ? err : new Error('Unknown error'));
    } finally {
      setIsLoading(false);
    }
  }, [enabled]); // Only depends on enabled

  // Initial fetch when IDs change (only if enabled)
  useEffect(() => {
    if (!enabled) return;
    fetchChanges();
  }, [threadId, agentId, turnIndex, enabled, fetchChanges]);

  // DISABLED: Polling removed to prevent request spam
  // Data will only update on initial mount or manual refetch
  // useEffect(() => {
  //   if (!enabled || !threadId) return;
  //   const interval = setInterval(() => fetchChanges(), 5000);
  //   return () => clearInterval(interval);
  // }, [enabled, threadId]);

  const summary = useMemo(() => {
    if (!data) return null;
    return {
      additions: data.additions,
      deletions: data.deletions,
      totalFiles: data.totalFiles,
      display: `+${data.additions} -${data.deletions}`,
    };
  }, [data]);

  return {
    data,
    summary,
    isLoading,
    error,
    refetch: fetchChanges,
  };
}
