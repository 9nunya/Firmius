'use client';

import React, { useState } from 'react';
import { motion, AnimatePresence } from 'framer-motion';
import { cn } from '@/lib/utils';
import { ToolCallRow, type ToolStatus } from './ToolCallRow';

export interface ToolCallBatchItem {
  id: string;
  description: string;
  status: ToolStatus;
  filePath?: string;
  operation?: string;
  summary?: string;
  error?: string;
  durationMs?: number;
  details?: React.ReactNode;
}

export interface ToolCallBatchProps {
  items: ToolCallBatchItem[];
  turnId: string;
  className?: string;
}

export function ToolCallBatch({ items, turnId, className }: ToolCallBatchProps) {
  const [expandedItems, setExpandedItems] = useState<Set<string>>(new Set());

  // Group similar items for batch display
  const groupedItems = React.useMemo(() => {
    const groups: Array<{
      type: 'single' | 'batch';
      description: string;
      items: ToolCallBatchItem[];
    }> = [];

    let currentGroup: ToolCallBatchItem[] = [];
    let currentDescription = '';

    for (const item of items) {
      const baseDesc = item.description;
      
      if (currentGroup.length === 0 || baseDesc === currentDescription) {
        currentGroup.push(item);
        currentDescription = baseDesc;
      } else {
        groups.push({
          type: currentGroup.length === 1 ? 'single' : 'batch',
          description: currentDescription,
          items: [...currentGroup],
        });
        currentGroup = [item];
        currentDescription = baseDesc;
      }
    }

    if (currentGroup.length > 0) {
      groups.push({
        type: currentGroup.length === 1 ? 'single' : 'batch',
        description: currentDescription,
        items: [...currentGroup],
      });
    }

    return groups;
  }, [items]);

  const toggleExpanded = (itemId: string) => {
    setExpandedItems(prev => {
      const next = new Set(prev);
      if (next.has(itemId)) {
        next.delete(itemId);
      } else {
        next.add(itemId);
      }
      return next;
    });
  };

  const allComplete = items.every(item => item.status === 'done' || item.status === 'error');
  const allSuccess = items.every(item => item.status === 'done');
  const hasErrors = items.some(item => item.status === 'error');
  const totalCount = items.length;
  const completedCount = items.filter(item => item.status === 'done' || item.status === 'error').length;

  return (
    <div className={cn('space-y-1', className)}>
      {groupedItems.map((group, groupIndex) => {
        if (group.type === 'single') {
          const item = group.items[0]!;
          return (
            <ToolCallRow
              key={item.id}
              description={item.description}
              status={item.status}
              filePath={item.filePath}
              operation={item.operation}
              summary={item.summary}
              error={item.error}
              durationMs={item.durationMs}
              onExpand={item.details ? () => toggleExpanded(item.id) : undefined}
              isExpanded={expandedItems.has(item.id)}
            >
              {item.details}
            </ToolCallRow>
          );
        }

        // Batch display
        const isExpanded = expandedItems.has(`batch-${groupIndex}`);
        const batchComplete = group.items.every(item => item.status === 'done' || item.status === 'error');
        const batchSuccess = group.items.every(item => item.status === 'done');
        const batchHasErrors = group.items.some(item => item.status === 'error');

        return (
          <div key={`batch-${groupIndex}`} className="space-y-1">
            {/* Batch header */}
            <motion.div
              initial={{ opacity: 0, x: -4 }}
              animate={{ opacity: 1, x: 0 }}
              transition={{ duration: 0.15, ease: 'easeOut' }}
              className={cn(
                'flex items-center gap-2 py-1 px-0 text-sm cursor-pointer hover:bg-accent/50 rounded-sm',
                batchHasErrors && 'text-red-400'
              )}
              onClick={() => toggleExpanded(`batch-${groupIndex}`)}
            >
              {/* Status indicator */}
              <span className="flex-shrink-0 w-4 flex justify-center">
                {batchComplete ? (
                  batchSuccess ? (
                    <svg className="w-3 h-3 text-green-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={3} d="M5 13l4 4L19 7" />
                    </svg>
                  ) : (
                    <svg className="w-3 h-3 text-red-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={3} d="M6 18L18 6M6 6l12 12" />
                    </svg>
                  )
                ) : (
                  <span className="w-2 h-2 rounded-full bg-amber-500 animate-pulse" />
                )}
              </span>

              {/* Description */}
              <span className="text-muted-foreground">{group.description}</span>
              <span className="text-foreground font-medium">{group.items.length} items</span>

              {/* Expand indicator */}
              <svg
                className={cn(
                  'w-3 h-3 text-muted-foreground ml-auto flex-shrink-0 transition-transform duration-150',
                  isExpanded && 'rotate-180'
                )}
                fill="none"
                viewBox="0 0 24 24"
                stroke="currentColor"
              >
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M19 9l-7 7-7-7" />
              </svg>
            </motion.div>

            {/* Expanded batch items */}
            <AnimatePresence>
              {isExpanded && (
                <motion.div
                  initial={{ opacity: 0, height: 0 }}
                  animate={{ opacity: 1, height: 'auto' }}
                  exit={{ opacity: 0, height: 0 }}
                  transition={{ duration: 0.15, ease: 'easeOut' }}
                  className="ml-6 pl-2 border-l border-border space-y-1"
                >
                  {group.items.map((item, itemIndex) => (
                    <motion.div
                      key={item.id}
                      initial={{ opacity: 0, x: -4 }}
                      animate={{ opacity: 1, x: 0 }}
                      transition={{
                        duration: 0.15,
                        ease: 'easeOut',
                        delay: itemIndex * 0.05,
                      }}
                    >
                      <ToolCallRow
                        description=""
                        status={item.status}
                        filePath={item.filePath}
                        operation={item.operation}
                        summary={item.summary}
                        error={item.error}
                        durationMs={item.durationMs}
                        onExpand={item.details ? () => toggleExpanded(item.id) : undefined}
                        isExpanded={expandedItems.has(item.id)}
                      >
                        {item.details}
                      </ToolCallRow>
                    </motion.div>
                  ))}
                </motion.div>
              )}
            </AnimatePresence>
          </div>
        );
      })}
    </div>
  );
}
