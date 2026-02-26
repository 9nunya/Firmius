'use client';

import React, { useState } from 'react';
import useAppStore from '@/stores/app-store';
import { CreateThreadModal } from '@/components/modals/CreateThreadModal';
import type { Thread } from '@/types';
import { cn } from '@/lib/utils';
import { ArrowLeft, Plus, X, Trash2, Loader2 } from 'lucide-react';
import { ThemeToggle } from './ThemeToggle';

interface SidebarProps {
  className?: string;
  isMobileOpen?: boolean;
  setIsMobileOpen?: (open: boolean) => void;
}

export function Sidebar({ className, isMobileOpen: externalIsMobileOpen, setIsMobileOpen: externalSetIsMobileOpen }: SidebarProps): React.ReactElement {
  const [isModalOpen, setIsModalOpen] = useState(false);
  const [internalIsMobileOpen, setInternalIsMobileOpen] = useState(false);

  // Use external state if provided, otherwise use internal state
  const isMobileOpen = externalIsMobileOpen !== undefined ? externalIsMobileOpen : internalIsMobileOpen;
  const setIsMobileOpen = externalSetIsMobileOpen || setInternalIsMobileOpen;
  const { threads, activeThreadId, selectThread, loadThreads, focusAgent, activeAgentId, sidebarTab, setSidebarTab } = useAppStore();

  // Initial load of threads on mount
  const [initialLoadDone, setInitialLoadDone] = React.useState(false);

  // Ensure threads are loaded when tab changes to threads
  React.useEffect(() => {
    if (sidebarTab === 'threads') {
      loadThreads().then(() => setInitialLoadDone(true));
    }
  }, [sidebarTab]);

  // Handle thread selection from URL - runs once after initial load
  React.useEffect(() => {
    if (!initialLoadDone) return;

    const params = new URLSearchParams(window.location.search);
    const threadId = params.get('thread');
    if (threadId) {
      // Verify thread exists before selecting
      const currentThreads = useAppStore.getState().threads;
      const threadExists = currentThreads.some(t => t.id === threadId);
      if (threadExists) {
        selectThread(threadId);
      } else {
        // Remove invalid thread ID from URL
        window.history.replaceState({}, '', window.location.pathname);
      }
    }
  }, [initialLoadDone]);

  const isSubagentFocused = React.useMemo(() => {
    if (!activeAgentId || !activeThreadId) return false;

    const activeThread = threads.find(t => t.id === activeThreadId);
    if (!activeThread?.leadAgentId) return false;

    return activeAgentId !== activeThread.leadAgentId;
  }, [activeAgentId, activeThreadId, threads]);

  const parentAgentId = React.useMemo(() => {
    if (!activeAgentId || !isSubagentFocused) return null;

    const agents = useAppStore.getState().agents;
    const agent = agents.find(a => a.id === activeAgentId);
    return agent?.parentId || null;
  }, [activeAgentId, isSubagentFocused]);

  const handleSelectThread = (threadId: string) => {
    // Always switch to threads tab when selecting a thread
    if (sidebarTab !== 'threads') {
      setSidebarTab('threads');
    }
    void selectThread(threadId);
    setIsMobileOpen(false);
  };

  return (
    <>
      {/* Mobile Overlay */}
      {isMobileOpen && (
        <div
          className="md:hidden fixed inset-0 bg-black/50 z-40 backdrop-blur-sm"
          onClick={() => setIsMobileOpen(false)}
        />
      )}

      <div
        className={cn(
          'flex w-64 flex-col border-r border-border bg-card',
          'transition-transform duration-300 ease-in-out',
          'fixed inset-y-0 left-0 z-50 md:relative md:translate-x-0',
          isMobileOpen ? 'translate-x-0' : '-translate-x-full',
          className
        )}
      >
        {/* Header */}
        <div className="flex flex-col gap-2 p-3 border-b border-border">
          <div className="flex items-center justify-between px-1">
            <span className="font-semibold text-sm tracking-tight">Firmius</span>
          </div>
          {/* Tab bar */}
          <div className="flex gap-1">
            <button
              type="button"
              onClick={() => setSidebarTab('threads')}
              className={cn(
                'flex-1 rounded px-2 py-1 text-sm transition-colors',
                sidebarTab === 'threads'
                  ? 'bg-primary text-primary-foreground'
                  : 'hover:bg-accent/50 text-muted-foreground'
              )}
            >
              Threads
            </button>
            <button
              type="button"
              onClick={() => setSidebarTab('settings')}
              className={cn(
                'flex-1 rounded px-2 py-1 text-sm transition-colors',
                sidebarTab === 'settings'
                  ? 'bg-primary text-primary-foreground'
                  : 'hover:bg-accent/50 text-muted-foreground'
              )}
            >
              Settings
            </button>
          </div>
        </div>

        {isSubagentFocused && parentAgentId && (
          <button
            type="button"
            onClick={() => focusAgent(parentAgentId)}
            className="mx-2 mt-3 flex items-center gap-2 px-3 py-2 text-sm bg-indigo-500/10 hover:bg-indigo-500/20 border border-indigo-500/30 transition-all duration-200 text-indigo-400"
          >
            <ArrowLeft size={16} />
            <span className="font-medium">Back to Parent Agent</span>
          </button>
        )}

        {/* Thread list content - always shown */}
        <div className="flex-1 overflow-y-auto">
          <div className="flex flex-col">
            {/* New Thread Button */}
            <button
              type="button"
              onClick={() => {
                setIsModalOpen(true);
                setIsMobileOpen(false);
              }}
              className="mx-2 mt-2 mb-2 flex items-center justify-center gap-2 bg-primary px-4 py-2 text-sm font-medium text-primary-foreground transition-colors hover:bg-primary/90"
            >
              <Plus size={16} />
              New Thread
            </button>

            {/* Thread List */}
            <div className="flex flex-col">
              {threads.length === 0 ? (
                <div className="px-4 py-8 text-center text-sm text-muted-foreground">
                  No threads yet
                </div>
              ) : (
                [...threads]
                  .sort((a: Thread, b: Thread) => b.checkpointedAt.getTime() - a.checkpointedAt.getTime())
                  .map((thread: Thread) => (
                    <div key={thread.id} className="group relative">
                      <button
                        type="button"
                        onClick={() => handleSelectThread(thread.id)}
                        className={cn(
                          'w-full flex flex-col items-start px-4 py-3 text-left transition-colors',
                          activeThreadId === thread.id
                            ? 'bg-accent text-accent-foreground'
                            : 'hover:bg-accent/50',
                          'pr-12'
                        )}
                      >
                        <div className="flex items-center gap-2 w-full pr-2">
                          <div className="truncate text-sm font-medium flex-1">{thread.title}</div>
                        </div>
                        <div className="flex items-center gap-1.5 mt-0.5 max-w-full">
                          <span className="text-[9px] font-bold uppercase tracking-wider text-primary/70 bg-primary/5 px-1 py-0.5 rounded border border-primary/10 flex-shrink-0">
                            {thread.hostType || 'local'}
                          </span>
                          <div className="truncate text-[10px] text-muted-foreground opacity-70">
                            {thread.rootCwd}
                          </div>
                        </div>
                      </button>
                      <button
                        onClick={(e) => {
                          e.stopPropagation();
                          if (confirm('Delete this thread and all its history?')) {
                            useAppStore.getState().deleteThread(thread.id);
                          }
                        }}
                        className="absolute right-1 top-1/2 -translate-y-1/2 p-2 opacity-100 md:opacity-0 md:group-hover:opacity-100 text-muted-foreground hover:text-destructive transition-all"
                        title="Delete thread"
                      >
                        <Trash2 size={14} />
                      </button>
                    </div>
                  ))
              )}
            </div>
          </div>
        </div>

        {/* Theme Toggle in Footer */}
        <div className="border-t border-border p-3">
          <ThemeToggle className="w-full justify-center" />
        </div>
      </div>

      <CreateThreadModal isOpen={isModalOpen} onClose={() => setIsModalOpen(false)} />
    </>
  );
}

export default Sidebar;
