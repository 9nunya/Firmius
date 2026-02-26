'use client';

import React, { useMemo } from 'react';
import { cn } from '@/lib/utils';
import { ArrowLeft, Menu, Users } from 'lucide-react';
import useAppStore from '@/stores/app-store';

interface MobileTitleBarProps {
  className?: string;
  onToggleSidebar?: () => void;
  onToggleRightSidebar?: () => void;
}

export function MobileTitleBar({ className, onToggleSidebar, onToggleRightSidebar }: MobileTitleBarProps): React.ReactElement | null {
  const { activeThreadId, threads, activeAgentId, agents, focusAgent } = useAppStore();
  const [isMobile, setIsMobile] = React.useState(false);

  React.useEffect(() => {
    const checkMobile = () => setIsMobile(window.innerWidth < 768);
    checkMobile();
    window.addEventListener('resize', checkMobile);
    return () => window.removeEventListener('resize', checkMobile);
  }, []);

  // Get thread title
  const thread = threads.find(t => t.id === activeThreadId);
  const threadTitle = thread?.title || 'Firmius';

  // Determine if we're on subagent or lead agent
  const displayAgent = useMemo(() => {
    if (activeAgentId) {
      return agents.find(a => a.id === activeAgentId);
    }

    if (activeThreadId && thread?.leadAgentId) {
      return agents.find(a => a.id === thread.leadAgentId);
    }

    return null;
  }, [activeAgentId, activeThreadId, thread, agents]);

  // Show subagent title if focused on subagent, otherwise thread title
  const title = useMemo(() => {
    // Check if we're focused on subagent (not lead agent)
    if (displayAgent && activeAgentId && thread?.leadAgentId !== activeAgentId) {
      return displayAgent.readableName;
    }

    // Default to thread title
    return threadTitle;
  }, [displayAgent, activeAgentId, thread, threadTitle]);

  // Find parent agent ID for back button
  const parentAgentId = useMemo(() => {
    if (!displayAgent || !activeAgentId) return null;

    const thread = threads.find(t => t.id === activeThreadId);
    if (!thread?.leadAgentId) return null;

    // If not on lead agent, go back to parent
    if (activeAgentId !== thread.leadAgentId) {
      return displayAgent.parentId || thread.leadAgentId;
    }

    return null;
  }, [displayAgent, activeAgentId, threads, activeThreadId]);

  // Only render on mobile
  if (!isMobile) return null;

  return (
    <div className={cn(
      "sticky top-0 z-20 bg-card border-b border-border px-4 py-3 pt-[env(safe-area-inset-top)]",
      className
    )}>
      <div className="flex items-center justify-between gap-3">
        {/* Left: Sidebar toggle button */}
        <button
          type="button"
          onClick={onToggleSidebar}
          className="flex items-center justify-center p-2 bg-muted hover:bg-muted/80 border border-border transition-colors shrink-0"
          aria-label="Toggle Sidebar"
        >
          <Menu size={20} />
        </button>

        {/* Center/Right: Title (aligned right when no back button) */}
        <div className="flex-1 flex items-center justify-end gap-2">
          {parentAgentId && (
            <button
              type="button"
              onClick={() => focusAgent(parentAgentId)}
              className="flex items-center gap-2 px-3 py-2 text-sm bg-indigo-500/10 hover:bg-indigo-500/20 border border-indigo-500/30 rounded-md transition-all duration-200 text-indigo-400"
            >
              <ArrowLeft size={16} />
              <span className="font-medium">Back</span>
            </button>
          )}

          <h1 className="text-base font-bold text-foreground truncate text-right flex-1 min-w-0">
            {title}
          </h1>
        </div>
      </div>
    </div>
  );
}

export default MobileTitleBar;
