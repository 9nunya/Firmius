'use client';

import React, { useState, useEffect, useCallback } from 'react';
import { CheckCircle2, Clock, AlertCircle, ChevronUp, ChevronDown, X, Loader2, ListTodo } from 'lucide-react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { motion, AnimatePresence } from 'framer-motion';

interface TodoItem {
  id: number;
  content: string;
  status: 'pending' | 'in_progress' | 'completed';
  priority: 'high' | 'medium' | 'low';
  createdAt: number;
}

export function TodoPanel() {
  const { activeAgentId, agents } = useAppStore();
  const [isOpen, setIsOpen] = useState(false);
  const [isMobile, setIsMobile] = useState(false);
  const [todos, setTodos] = useState<TodoItem[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Check if mobile
  useEffect(() => {
    const checkMobile = () => {
      setIsMobile(window.innerWidth < 768);
    };
    checkMobile();
    window.addEventListener('resize', checkMobile);
    return () => window.removeEventListener('resize', checkMobile);
  }, []);

  // Fetch todos when active agent changes
  const fetchTodos = useCallback(async () => {
    if (!activeAgentId) {
      setTodos([]);
      return;
    }

    setIsLoading(true);
    setError(null);

    try {
      // Find the agent's thread
      const agent = agents.find(a => a.id === activeAgentId);
      if (!agent?.threadId) {
        setTodos([]);
        return;
      }

      const response = await fetch(`/api/threads/${agent.threadId}/agents/${activeAgentId}/todos`);
      
      if (!response.ok) {
        throw new Error(`Failed to fetch todos: ${response.statusText}`);
      }

      const data = await response.json();
      setTodos(data.todos || []);
    } catch (err) {
      console.error('Error fetching todos:', err);
      setError(err instanceof Error ? err.message : 'Failed to load todos');
    } finally {
      setIsLoading(false);
    }
  }, [activeAgentId, agents]);

  // Initial fetch and poll for updates
  useEffect(() => {
    fetchTodos();
    
    // Poll every 5 seconds when open
    const interval = setInterval(() => {
      if (activeAgentId) {
        fetchTodos();
      }
    }, 5000);

    return () => clearInterval(interval);
  }, [activeAgentId, fetchTodos]);

  // Get active agent name
  const activeAgent = activeAgentId ? agents.find(a => a.id === activeAgentId) : null;
  const agentName = activeAgent?.readableName || activeAgent?.purpose || 'Agent';

  // Count by status
  const counts = {
    pending: todos.filter(t => t.status === 'pending').length,
    in_progress: todos.filter(t => t.status === 'in_progress').length,
    completed: todos.filter(t => t.status === 'completed').length,
  };

  const totalCount = todos.length;
  const hasTodos = totalCount > 0;

  // Get status icon
  const getStatusIcon = (status: TodoItem['status']) => {
    switch (status) {
      case 'completed':
        return <CheckCircle2 size={14} className="text-green-500" />;
      case 'in_progress':
        return <Loader2 size={14} className="text-blue-500 animate-spin" />;
      case 'pending':
        return <Clock size={14} className="text-amber-500" />;
      default:
        return <Clock size={14} className="text-muted-foreground" />;
    }
  };

  // Get status color
  const getStatusColor = (status: TodoItem['status']) => {
    switch (status) {
      case 'completed':
        return 'bg-green-500/10 border-green-500/30';
      case 'in_progress':
        return 'bg-blue-500/10 border-blue-500/30';
      case 'pending':
        return 'bg-amber-500/10 border-amber-500/30';
      default:
        return 'bg-muted/30 border-border/30';
    }
  };

  // Get priority badge
  const getPriorityBadge = (priority: TodoItem['priority']) => {
    const colors = {
      high: 'bg-red-500/10 text-red-600 dark:text-red-400',
      medium: 'bg-amber-500/10 text-amber-600 dark:text-amber-400',
      low: 'bg-slate-500/10 text-slate-600 dark:text-slate-400',
    };

    return (
      <span className={cn("text-[9px] font-bold uppercase tracking-wider px-1.5 py-0.5 rounded", colors[priority])}>
        {priority}
      </span>
    );
  };

  // Toggle panel visibility
  const togglePanel = () => {
    setIsOpen(!isOpen);
  };

  // Close panel
  const closePanel = () => {
    setIsOpen(false);
  };

  // Don't render if no active agent
  if (!activeAgentId) {
    return null;
  }

  return (
    <>
      {/* Mobile Bottom Sheet */}
      <AnimatePresence>
        {isMobile && isOpen && (
          <>
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              exit={{ opacity: 0 }}
              className="fixed inset-0 bg-black/50 z-40"
              onClick={closePanel}
            />
            <motion.div
              initial={{ y: "100%" }}
              animate={{ y: 0 }}
              exit={{ y: "100%" }}
              transition={{ type: "spring", damping: 25, stiffness: 300 }}
              className="fixed bottom-0 left-0 right-0 z-50 bg-background border-t border-border rounded-t-xl max-h-[80vh]"
            >
              {/* Header */}
              <div className="flex items-center justify-between px-4 py-3 border-b border-border">
                <div className="flex items-center gap-2">
                  <ListTodo size={18} className="text-primary" />
                  <div>
                    <h3 className="font-medium text-sm">{agentName}</h3>
                    <p className="text-xs text-muted-foreground">
                      {hasTodos ? `${counts.in_progress} active, ${counts.completed} done` : 'No todos'}
                    </p>
                  </div>
                </div>
                <button
                  onClick={closePanel}
                  className="p-2 hover:bg-accent rounded-lg transition-colors"
                >
                  <X size={18} />
                </button>
              </div>

              {/* Content */}
              <div className="p-4 overflow-y-auto" style={{ maxHeight: 'calc(80vh - 60px)' }}>
                {isLoading ? (
                  <div className="flex items-center justify-center py-8">
                    <Loader2 size={24} className="animate-spin text-muted-foreground" />
                  </div>
                ) : error ? (
                  <div className="flex items-center gap-2 text-red-500 py-4">
                    <AlertCircle size={16} />
                    <span className="text-sm">{error}</span>
                  </div>
                ) : !hasTodos ? (
                  <div className="flex flex-col items-center justify-center py-8 text-muted-foreground">
                    <ListTodo size={32} className="mb-2 opacity-30" />
                    <p className="text-sm">No todos for {agentName}</p>
                  </div>
                ) : (
                  <div className="space-y-2">
                    {todos.map((todo) => (
                      <div
                        key={todo.id}
                        className={cn(
                          "flex items-start gap-3 p-3 rounded-lg border transition-all",
                          getStatusColor(todo.status)
                        )}
                      >
                        <div className="shrink-0 mt-0.5">{getStatusIcon(todo.status)}</div>
                        <div className="flex-1 min-w-0">
                          <p className={cn(
                            "text-sm",
                            todo.status === 'completed' && "line-through text-muted-foreground"
                          )}>
                            {todo.content}
                          </p>
                          <div className="flex items-center gap-2 mt-1">
                            {getPriorityBadge(todo.priority)}
                            <span className="text-[10px] text-muted-foreground">
                              {new Date(todo.createdAt).toLocaleDateString()}
                            </span>
                          </div>
                        </div>
                      </div>
                    ))}
                  </div>
                )}
              </div>
            </motion.div>
          </>
        )}
      </AnimatePresence>

      {/* Desktop Floating Panel */}
      {!isMobile && (
        <motion.div
          initial={false}
          animate={isOpen ? { width: 320, opacity: 1 } : { width: 'auto', opacity: 1 }}
          className={cn(
            "fixed bottom-4 right-4 z-50 bg-background border border-border rounded-xl shadow-lg overflow-hidden",
            isOpen ? "" : "cursor-pointer hover:bg-accent/50 transition-colors"
          )}
          onClick={!isOpen ? togglePanel : undefined}
        >
          {/* Collapsed State */}
          {!isOpen && (
            <div className="flex items-center gap-2 px-4 py-3">
              <div className="relative">
                <ListTodo size={20} className="text-primary" />
                {hasTodos && (
                  <span className="absolute -top-1 -right-1 w-4 h-4 bg-primary text-primary-foreground text-[10px] font-bold rounded-full flex items-center justify-center">
                    {totalCount}
                  </span>
                )}
              </div>
              <span className="font-medium text-sm">Todos</span>
              <ChevronUp size={16} className="text-muted-foreground ml-2" />
            </div>
          )}

          {/* Expanded State */}
          <AnimatePresence>
            {isOpen && (
              <motion.div
                initial={{ height: 0 }}
                animate={{ height: 'auto' }}
                exit={{ height: 0 }}
                className="flex flex-col"
              >
                {/* Header */}
                <div className="flex items-center justify-between px-4 py-3 border-b border-border">
                  <div className="flex items-center gap-2">
                    <ListTodo size={18} className="text-primary" />
                    <div>
                      <h3 className="font-medium text-sm">{agentName}</h3>
                      <p className="text-xs text-muted-foreground">
                        {hasTodos ? `${counts.in_progress} active, ${counts.completed} done` : 'No todos'}
                      </p>
                    </div>
                  </div>
                  <button
                    onClick={(e) => {
                      e.stopPropagation();
                      closePanel();
                    }}
                    className="p-2 hover:bg-accent rounded-lg transition-colors"
                  >
                    <ChevronDown size={18} />
                  </button>
                </div>

                {/* Content */}
                <div className="p-3 overflow-y-auto" style={{ maxHeight: 400 }}>
                  {isLoading ? (
                    <div className="flex items-center justify-center py-8">
                      <Loader2 size={24} className="animate-spin text-muted-foreground" />
                    </div>
                  ) : error ? (
                    <div className="flex items-center gap-2 text-red-500 py-4">
                      <AlertCircle size={16} />
                      <span className="text-sm">{error}</span>
                    </div>
                  ) : !hasTodos ? (
                    <div className="flex flex-col items-center justify-center py-8 text-muted-foreground">
                      <ListTodo size={32} className="mb-2 opacity-30" />
                      <p className="text-sm">No todos for {agentName}</p>
                    </div>
                  ) : (
                    <div className="space-y-2">
                      {todos.map((todo) => (
                        <div
                          key={todo.id}
                          className={cn(
                            "flex items-start gap-2 p-2.5 rounded-lg border transition-all",
                            getStatusColor(todo.status)
                          )}
                        >
                          <div className="shrink-0 mt-0.5">{getStatusIcon(todo.status)}</div>
                          <div className="flex-1 min-w-0">
                            <p className={cn(
                              "text-xs",
                              todo.status === 'completed' && "line-through text-muted-foreground"
                            )}>
                              {todo.content}
                            </p>
                            <div className="flex items-center gap-2 mt-1">
                              {getPriorityBadge(todo.priority)}
                            </div>
                          </div>
                        </div>
                      ))}
                    </div>
                  )}
                </div>
              </motion.div>
            )}
          </AnimatePresence>
        </motion.div>
      )}

      {/* Mobile Trigger Button */}
      {isMobile && !isOpen && (
        <motion.button
          initial={{ scale: 0.9, opacity: 0 }}
          animate={{ scale: 1, opacity: 1 }}
          className="fixed bottom-4 right-4 z-50 flex items-center gap-2 px-4 py-3 bg-primary text-primary-foreground rounded-full shadow-lg"
          onClick={togglePanel}
        >
          <div className="relative">
            <ListTodo size={20} />
            {hasTodos && (
              <span className="absolute -top-1 -right-1 w-4 h-4 bg-white text-primary text-[10px] font-bold rounded-full flex items-center justify-center">
                {totalCount}
              </span>
            )}
          </div>
          <span className="font-medium text-sm">Todos</span>
        </motion.button>
      )}
    </>
  );
}

export default TodoPanel;
