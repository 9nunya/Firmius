'use client';

import React, { useEffect, useState, useCallback, useRef } from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { motion, AnimatePresence } from 'framer-motion';

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

interface ChangesTabProps {
  className?: string;
}

export function ChangesTab({ className }: ChangesTabProps) {
  const { threads, activeThreadId, activeAgentId } = useAppStore();
  const [data, setData] = useState<ChangesData | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [expandedFile, setExpandedFile] = useState<string | null>(null);

  const activeThread = threads.find(t => t.id === activeThreadId);
  
  // Determine which agent's changes to show
  // If focused on a subagent, show that agent's changes
  // If focused on lead (or no focus), show all thread changes
  const targetAgentId = activeAgentId;
  const isLeadView = !activeAgentId || activeAgentId === activeThread?.leadAgentId;

  // Use refs to avoid dependency issues with intervals
  const stateRef = useRef({ activeThreadId, targetAgentId, isLeadView });
  stateRef.current = { activeThreadId, targetAgentId, isLeadView };

  const fetchChanges = useCallback(async () => {
    const { activeThreadId: threadId, targetAgentId: agentId, isLeadView: leadView } = stateRef.current;
    
    if (!threadId) {
      setData(null);
      return;
    }

    setIsLoading(true);
    setError(null);

    try {
      let url: string;
      
      if (agentId && !leadView) {
        // Get changes for specific agent
        url = `/api/threads/${threadId}/agents/${agentId}/changes`;
      } else {
        // Get all thread changes (for lead view)
        url = `/api/threads/${threadId}/changes`;
      }

      const response = await fetch(url);
      
      if (response.status === 404) {
        // No changes yet - show empty state
        setData({ additions: 0, deletions: 0, totalFiles: 0, files: [] });
        setError(null);
        return;
      }
      
      if (!response.ok) {
        throw new Error('Failed to fetch changes');
      }
      
      const result = await response.json();
      setData(result);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load changes');
      setData(null);
    } finally {
      setIsLoading(false);
    }
  }, []); // No dependencies - uses ref

  // Fetch once on mount and when IDs change
  useEffect(() => {
    fetchChanges();
  }, [activeThreadId, targetAgentId, isLeadView]);

  // DISABLED: Polling removed to prevent request spam
  // Changes will only update when user manually refreshes or re-opens the tab
  // const workingAgentId = useAppStore((state) => {
  //   const workingAgent = state.agents.find(a => a.threadId === state.activeThreadId && a.status === 'working');
  //   return workingAgent?.id || null;
  // });

  const getOperationIcon = (operation: string) => {
    switch (operation) {
      case 'write':
        return <span className="text-[10px] text-blue-500">NEW</span>;
      case 'replace':
        return <span className="text-[10px] text-amber-500">MOD</span>;
      case 'patch':
        return <span className="text-[10px] text-purple-500">PATCH</span>;
      case 'apply_diff':
        return <span className="text-[10px] text-green-500">DIFF</span>;
      default:
        return <span className="text-[10px] text-muted-foreground">EDIT</span>;
    }
  };

  if (isLoading && !data) {
    return (
      <div className={cn('flex flex-col h-full', className)}>
        <div className="flex items-center justify-between px-3 py-2 border-b border-border">
          <span className="text-xs font-medium text-muted-foreground">Changes</span>
        </div>
        <div className="flex-1 flex items-center justify-center">
          <span className="text-xs text-muted-foreground animate-pulse">Loading...</span>
        </div>
      </div>
    );
  }

  if (error) {
    return (
      <div className={cn('flex flex-col h-full', className)}>
        <div className="flex items-center justify-between px-3 py-2 border-b border-border">
          <span className="text-xs font-medium text-muted-foreground">Changes</span>
        </div>
        <div className="flex-1 flex items-center justify-center p-4">
          <span className="text-xs text-red-500">{error}</span>
        </div>
      </div>
    );
  }

  return (
    <div className={cn('flex flex-col h-full', className)}>
      {/* Header with totals */}
      <div className="flex items-center justify-between px-3 py-2 border-b border-border">
        <span className="text-xs font-medium text-muted-foreground">
          {isLeadView ? 'Thread Changes' : 'Agent Changes'}
        </span>
        {data && data.totalFiles > 0 && (
          <div className="text-[10px] font-mono">
            <span className="text-green-500">+{data.additions}</span>
            <span className="text-red-500 ml-1">-{data.deletions}</span>
            <span className="text-muted-foreground ml-1">({data.totalFiles} files)</span>
          </div>
        )}
      </div>

      {/* File changes list */}
      <div className="flex-1 overflow-y-auto p-2 space-y-1">
        <AnimatePresence mode="popLayout">
          {!data || data.files.length === 0 ? (
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              className="text-center py-8 text-xs text-muted-foreground"
            >
              No file changes
            </motion.div>
          ) : (
            data.files.map((file, index) => (
              <motion.div
                key={`${file.file}-${file.timestamp}`}
                layout
                initial={{ opacity: 0, y: 10 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, height: 0 }}
                transition={{ delay: index * 0.03 }}
                className="space-y-1"
              >
                <button
                  onClick={() => setExpandedFile(expandedFile === file.file ? null : file.file)}
                  className={cn(
                    'w-full flex items-center justify-between px-2 py-1.5 rounded-sm',
                    'hover:bg-accent/50 transition-colors text-left'
                  )}
                >
                  <div className="flex items-center gap-2 min-w-0 flex-1">
                    {getOperationIcon(file.operation)}
                    <span className="text-xs font-mono truncate" title={file.file}>
                      {file.file.split('/').pop()}
                    </span>
                    <span className="text-[10px] text-muted-foreground truncate flex-shrink-0">
                      {file.file}
                    </span>
                  </div>
                  <div className="flex items-center gap-1 text-[10px] font-mono flex-shrink-0">
                    {file.additions > 0 && (
                      <span className="text-green-500">+{file.additions}</span>
                    )}
                    {file.deletions > 0 && (
                      <span className="text-red-500">-{file.deletions}</span>
                    )}
                    <svg
                      className={cn(
                        'w-3 h-3 text-muted-foreground transition-transform',
                        expandedFile === file.file && 'rotate-180'
                      )}
                      fill="none"
                      viewBox="0 0 24 24"
                      stroke="currentColor"
                    >
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 9l-7 7-7-7" />
                    </svg>
                  </div>
                </button>

                {/* Expanded details */}
                {expandedFile === file.file && (
                  <motion.div
                    initial={{ opacity: 0, height: 0 }}
                    animate={{ opacity: 1, height: 'auto' }}
                    exit={{ opacity: 0, height: 0 }}
                    className="pl-6 pr-2 py-1 space-y-1"
                  >
                    <div className="text-[10px] text-muted-foreground">
                      Full path: {file.file}
                    </div>
                    <div className="text-[10px] text-muted-foreground">
                      Operation: {file.operation}
                    </div>
                    <div className="text-[10px] text-muted-foreground">
                      {new Date(file.timestamp).toLocaleString()}
                    </div>
                  </motion.div>
                )}
              </motion.div>
            ))
          )}
        </AnimatePresence>
      </div>
    </div>
  );
}

export default ChangesTab;
