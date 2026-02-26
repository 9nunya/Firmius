'use client';

import React, { useMemo } from 'react';
import useAppStore from '@/stores/app-store';
import type { Agent } from '@firmius/shared/api';
import { cn } from '@/lib/utils';

interface FleetTabProps {
  className?: string;
}

function buildAgentTree(agents: Agent[], leadAgentId: string | null) {
  const tree: Map<string | null, Agent[]> = new Map();

  for (const agent of agents) {
    let parentKey: string | null = null;
    if (agent.isLead) {
      parentKey = null;
    } else {
      parentKey = agent.parentId ?? leadAgentId ?? null;
    }

    if (!tree.has(parentKey)) {
      tree.set(parentKey, []);
    }
    tree.get(parentKey)!.push(agent);
  }

  return tree;
}

function getStatusDot(status: string) {
  switch (status) {
    case 'working':
      return <span className="w-1.5 h-1.5 rounded-full bg-amber-500" />;
    case 'idle':
      return <span className="w-1.5 h-1.5 rounded-full bg-green-500" />;
    case 'error':
      return <span className="w-1.5 h-1.5 rounded-full bg-red-500" />;
    default:
      return <span className="w-1.5 h-1.5 rounded-full bg-muted-foreground" />;
  }
}

export function FleetTab({ className }: FleetTabProps) {
  const { agents, activeThreadId, activeAgentId, focusAgent, threads } = useAppStore();

  const activeThread = threads.find(t => t.id === activeThreadId);
  const leadAgentId = activeThread?.leadAgentId;

  const threadAgents = useMemo(() => {
    return agents.filter(a => a.threadId === activeThreadId);
  }, [agents, activeThreadId]);

  const agentTree = useMemo(() => {
    return buildAgentTree(threadAgents, leadAgentId ?? null);
  }, [threadAgents, leadAgentId]);

  const rootAgents = useMemo(() => {
    return agentTree.get(null) ?? threadAgents.filter(a => a.isLead || !a.parentId);
  }, [agentTree, threadAgents]);

  const stats = useMemo(() => {
    const working = threadAgents.filter(a => String(a.status) === 'working').length;
    const idle = threadAgents.filter(a => String(a.status) === 'idle').length;
    const error = threadAgents.filter(a => String(a.status) === 'error').length;
    return { working, idle, error, total: threadAgents.length };
  }, [threadAgents]);

  const renderAgent = (agent: Agent, depth: number = 0, renderedIds: Set<string> = new Set()): React.ReactElement | null => {
    // Prevent circular references
    if (renderedIds.has(agent.id)) {
      return null;
    }

    const newRenderedIds = new Set(renderedIds);
    newRenderedIds.add(agent.id);

    const children = agentTree.get(agent.id) ?? [];
    const isActive = activeAgentId === agent.id;

    return (
      <div key={agent.id}>
        <button
          onClick={() => focusAgent(isActive ? null : agent.id)}
          className={cn(
            'w-full flex items-center gap-2 px-2 py-1 text-left text-sm',
            'hover:bg-accent/50 rounded-sm transition-colors',
            isActive && 'bg-accent'
          )}
          style={{ paddingLeft: `${depth * 16 + 8}px` }}
        >
          {/* Status dot */}
          <span className="flex-shrink-0">{getStatusDot(agent.status)}</span>

          {/* Agent name */}
          <span className={cn(
            'truncate',
            agent.isLead && 'font-medium'
          )}>
            {agent.readableName}
          </span>

          {/* Lead indicator */}
          {agent.isLead && (
            <span className="text-[10px] text-muted-foreground">(LEAD)</span>
          )}
        </button>

        {/* Render children recursively */}
        {children.length > 0 && (
          <div className="mt-0.5">
            {children.map((child: Agent) => renderAgent(child, depth + 1, newRenderedIds))}
          </div>
        )}
      </div>
    );
  };

  if (!activeThreadId) {
    return null;
  }

  return (
    <div className={cn('flex flex-col h-full', className)}>
      {/* Header */}
      <div className="flex items-center justify-between px-3 py-2 border-b border-border">
        <span className="text-xs font-medium text-muted-foreground">Fleet</span>
        <span className="text-[10px] text-muted-foreground">
          {stats.working > 0 && `${stats.working} working, `}
          {stats.total} total
        </span>
      </div>

      {/* Agent tree */}
      <div className="flex-1 overflow-y-auto p-1">
        {threadAgents.length === 0 ? (
          <div className="text-center py-8 text-xs text-muted-foreground">
            No agents
          </div>
        ) : (
          <div className="space-y-0.5">
            {rootAgents.map(agent => renderAgent(agent)).filter(Boolean)}
          </div>
        )}
      </div>

      {/* Stats footer */}
      {stats.total > 0 && (
        <div className="px-3 py-2 border-t border-border text-[10px] text-muted-foreground">
          <div className="flex items-center gap-3">
            {stats.working > 0 && (
              <span className="flex items-center gap-1">
                <span className="w-1.5 h-1.5 rounded-full bg-amber-500" />
                {stats.working} working
              </span>
            )}
            {stats.idle > 0 && (
              <span className="flex items-center gap-1">
                <span className="w-1.5 h-1.5 rounded-full bg-green-500" />
                {stats.idle} idle
              </span>
            )}
            {stats.error > 0 && (
              <span className="flex items-center gap-1">
                <span className="w-1.5 h-1.5 rounded-full bg-red-500" />
                {stats.error} error
              </span>
            )}
          </div>
        </div>
      )}
     </div>
  );
}

export default FleetTab;
