'use client';

import React, { useState, useEffect } from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';

interface MobileBottomBarProps {
  className?: string;
}

export function MobileBottomBar({ className }: MobileBottomBarProps) {
  const {
    activeThreadId,
    openRightSidebar,
    setRightSidebarTab,
    messages,
    selectThread,
  } = useAppStore();

  const [isMobile, setIsMobile] = useState(false);

  // Detect mobile viewport
  useEffect(() => {
    const checkMobile = () => {
      setIsMobile(window.innerWidth < 768);
    };

    checkMobile();
    window.addEventListener('resize', checkMobile);
    return () => window.removeEventListener('resize', checkMobile);
  }, []);

  if (!isMobile || !activeThreadId) {
    return null;
  }

  // Find the last user message for undo
  const lastUserMessage = [...messages]
    .reverse()
    .find(m => m.isUser);

  // Find the last agent message for undo turn
  const lastAgentMessage = [...messages]
    .reverse()
    .find(m => !m.isUser && m.type === 'response');

  const handleOpenFleet = () => {
    setRightSidebarTab('fleet');
    openRightSidebar();
  };

  const handleOpenTodos = () => {
    setRightSidebarTab('todos');
    openRightSidebar();
  };

  const handleOpenChanges = () => {
    setRightSidebarTab('changes');
    openRightSidebar();
  };

  return (
    <div
      className={cn(
        'fixed bottom-0 left-0 right-0 z-30',
        'bg-background border-t border-border',
        'flex items-center justify-between px-4 py-2 h-[60px]',
        'safe-area-bottom',
        className
      )}
    >
      {/* Left side - Actions */}
      <div className="flex items-center gap-2">
        {lastUserMessage && (
          <button
            onClick={async () => {
              // Undo to last user message
              if (activeThreadId) {
                const response = await fetch(`/api/threads/${activeThreadId}/messages/${lastUserMessage.sequence}/undo`, {
                  method: 'POST',
                });
                if (response.ok) {
                  await selectThread(activeThreadId);
                }
              }
            }}
            className={cn(
              'p-2 rounded-sm text-xs font-medium',
              'bg-muted hover:bg-muted/80 transition-colors',
              'text-muted-foreground hover:text-foreground'
            )}
            title="Undo last message"
          >
            <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6" />
            </svg>
          </button>
        )}

        {lastAgentMessage && (
          <button
            onClick={async () => {
              // Undo last turn
              if (activeThreadId && lastAgentMessage.agentId) {
                const response = await fetch(`/api/threads/${activeThreadId}/agents/${lastAgentMessage.agentId}/undo-turn`, {
                  method: 'POST',
                });
                if (response.ok) {
                  await selectThread(activeThreadId);
                }
              }
            }}
            className={cn(
              'p-2 rounded-sm text-xs font-medium',
              'bg-muted hover:bg-muted/80 transition-colors',
              'text-muted-foreground hover:text-foreground'
            )}
            title="Undo last turn"
          >
            <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12.066 11.2a1 1 0 000 1.6l5.334 4A1 1 0 0019 16V8a1 1 0 00-1.6-.8l-5.333 4zM4.066 11.2a1 1 0 000 1.6l5.334 4A1 1 0 0011 16V8a1 1 0 00-1.6-.8l-5.334 4z" />
            </svg>
          </button>
        )}
      </div>

      {/* Right side - Open Sidebar buttons */}
      <div className="flex items-center gap-1">
        <button
          onClick={handleOpenFleet}
          className={cn(
            'px-2 py-2 rounded-sm text-xs font-medium',
            'bg-muted hover:bg-muted/80 transition-colors'
          )}
          title="Open Fleet"
        >
          <span className="hidden sm:inline">Fleet</span>
          <svg className="w-4 h-4 sm:hidden" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M17 20h5v-2a3 3 0 00-5.356-1.857M17 20H7m10 0v-2c0-.656-.126-1.283-.356-1.857M7 20H2v-2a3 3 0 015.356-1.857M7 20v-2c0-.656.126-1.283.356-1.857m0 0a5.002 5.002 0 019.288 0M15 7a3 3 0 11-6 0 3 3 0 016 0zm6 3a2 2 0 11-4 0 2 2 0 014 0zM7 10a2 2 0 11-4 0 2 2 0 014 0z" />
          </svg>
        </button>
        <button
          onClick={handleOpenTodos}
          className={cn(
            'px-2 py-2 rounded-sm text-xs font-medium',
            'bg-muted hover:bg-muted/80 transition-colors'
          )}
          title="Open Todos"
        >
          <span className="hidden sm:inline">Todos</span>
          <svg className="w-4 h-4 sm:hidden" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 5H7a2 2 0 00-2 2v12a2 2 0 002 2h10a2 2 0 002-2V7a2 2 0 00-2-2h-2M9 5a2 2 0 002 2h2a2 2 0 002-2M9 5a2 2 0 012-2h2a2 2 0 012 2m-6 9l2 2 4-4" />
          </svg>
        </button>
        <button
          onClick={handleOpenChanges}
          className={cn(
            'px-2 py-2 rounded-sm text-xs font-medium',
            'bg-muted hover:bg-muted/80 transition-colors'
          )}
          title="Open Changes"
        >
          <span className="hidden sm:inline">Changes</span>
          <svg className="w-4 h-4 sm:hidden" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9 12h6m-6 4h6m2 5H7a2 2 0 01-2-2V5a2 2 0 012-2h5.586a1 1 0 01.707.293l5.414 5.414a1 1 0 01.293.707V19a2 2 0 01-2 2z" />
          </svg>
        </button>
      </div>
    </div>
  );
}

export default MobileBottomBar;
