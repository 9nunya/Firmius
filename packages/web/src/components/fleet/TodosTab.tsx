'use client';

import React, { useEffect, useState, useCallback, useRef } from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { motion, AnimatePresence } from 'framer-motion';

interface Todo {
  id: string;
  content: string;
  status: 'pending' | 'in_progress' | 'done';
  priority?: 'low' | 'medium' | 'high';
  createdAt?: number;
}

interface TodosTabProps {
  className?: string;
}

export function TodosTab({ className }: TodosTabProps) {
  const { threads, activeThreadId, activeAgentId } = useAppStore();
  const [todos, setTodos] = useState<Todo[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const activeThread = threads.find(t => t.id === activeThreadId);
  
  // Determine which agent's todos to show
  // If focused on a subagent, show that agent's todos
  // If focused on lead (or no focus), show lead's todos
  const targetAgentId = activeAgentId || activeThread?.leadAgentId;

  // Use refs to avoid dependency issues with intervals
  const stateRef = useRef({ activeThreadId, targetAgentId });
  stateRef.current = { activeThreadId, targetAgentId };

  const fetchTodos = useCallback(async () => {
    const { activeThreadId: threadId, targetAgentId: agentId } = stateRef.current;
    
    if (!threadId || !agentId) {
      setTodos([]);
      return;
    }

    setIsLoading(true);
    setError(null);

    try {
      const response = await fetch(
        `/api/threads/${threadId}/agents/${agentId}/todos`
      );
      
      if (!response.ok) {
        if (response.status === 404) {
          // Agent not found - empty todos
          setTodos([]);
        } else {
          throw new Error('Failed to fetch todos');
        }
      } else {
        const data = await response.json();
        setTodos(data.todos || []);
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load todos');
      setTodos([]);
    } finally {
      setIsLoading(false);
    }
  }, []); // No dependencies - uses ref

  // Fetch on mount and when agent/thread changes
  useEffect(() => {
    fetchTodos();
  }, [activeThreadId, targetAgentId]);

  // DISABLED: Polling removed to prevent request spam
  // Todos will only update when user manually refreshes or re-opens the tab

  const getStatusIcon = (status: Todo['status']) => {
    switch (status) {
      case 'done':
        return <span className="w-2 h-2 rounded-full bg-green-500" />;
      case 'in_progress':
        return <span className="w-2 h-2 rounded-full bg-amber-500 animate-pulse" />;
      default:
        return <span className="w-2 h-2 rounded-full bg-muted-foreground" />;
    }
  };

  const getPriorityClass = (priority?: Todo['priority']) => {
    switch (priority) {
      case 'high':
        return 'border-l-2 border-l-red-500';
      case 'medium':
        return 'border-l-2 border-l-amber-500';
      default:
        return '';
    }
  };

  if (isLoading && todos.length === 0) {
    return (
      <div className={cn('flex flex-col h-full', className)}>
        <div className="flex items-center justify-between px-3 py-2 border-b border-border">
          <span className="text-xs font-medium text-muted-foreground">Todos</span>
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
          <span className="text-xs font-medium text-muted-foreground">Todos</span>
        </div>
        <div className="flex-1 flex items-center justify-center p-4">
          <span className="text-xs text-red-500">{error}</span>
        </div>
      </div>
    );
  }

  const pendingCount = todos.filter(t => t.status === 'pending').length;
  const inProgressCount = todos.filter(t => t.status === 'in_progress').length;
  const doneCount = todos.filter(t => t.status === 'done').length;

  return (
    <div className={cn('flex flex-col h-full', className)}>
      {/* Header */}
      <div className="flex items-center justify-between px-3 py-2 border-b border-border">
        <span className="text-xs font-medium text-muted-foreground">Todos</span>
        {todos.length > 0 && (
          <span className="text-[10px] text-muted-foreground">
            {pendingCount > 0 && `${pendingCount} pending `}
            {inProgressCount > 0 && `${inProgressCount} working `}
            {doneCount > 0 && `${doneCount} done`}
          </span>
        )}
      </div>

      {/* Todo list */}
      <div className="flex-1 overflow-y-auto p-2 space-y-1">
        <AnimatePresence mode="popLayout">
          {todos.length === 0 ? (
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              className="text-center py-8 text-xs text-muted-foreground"
            >
              No todos
            </motion.div>
          ) : (
            todos.map((todo, index) => (
              <motion.div
                key={todo.id}
                layout
                initial={{ opacity: 0, y: 10 }}
                animate={{ opacity: 1, y: 0 }}
                exit={{ opacity: 0, height: 0 }}
                transition={{ delay: index * 0.05 }}
                className={cn(
                  'flex items-start gap-2 px-2 py-1.5 rounded-sm hover:bg-accent/50 transition-colors',
                  getPriorityClass(todo.priority),
                  todo.status === 'done' && 'opacity-50'
                )}
              >
                <span className="flex-shrink-0 mt-1">{getStatusIcon(todo.status)}</span>
                <div className="flex-1 min-w-0">
                  <span className={cn(
                    'text-xs block truncate',
                    todo.status === 'done' && 'line-through text-muted-foreground'
                  )}>
                    {todo.content}
                  </span>
                </div>
              </motion.div>
            ))
          )}
        </AnimatePresence>
      </div>
    </div>
  );
}

export default TodosTab;
