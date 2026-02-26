'use client';

import React from 'react';
import { motion } from 'framer-motion';
import { cn } from '@/lib/utils';
import { Pencil, Loader2 } from 'lucide-react';

export type ToolStatus = 'preparing' | 'running' | 'done' | 'error';

export interface ToolCallRowProps {
  description: string;
  status: ToolStatus;
  filePath?: string;
  operation?: string;
  summary?: string;
  error?: string;
  durationMs?: number;
  onExpand?: () => void;
  isExpanded?: boolean;
  className?: string;
  children?: React.ReactNode;
}

const statusConfig = {
  preparing: {
    dot: 'bg-blue-500',
    pulse: true,
    icon: <Pencil className="w-3 h-3 text-blue-500" />,
    label: 'Writing',
  },
  running: {
    dot: 'bg-amber-500',
    pulse: true,
    icon: <Loader2 className="w-3 h-3 text-amber-500 animate-spin" />,
    label: 'Running',
  },
  done: {
    dot: 'bg-green-500',
    pulse: false,
    icon: (
      <svg className="w-3 h-3 text-green-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={3} d="M5 13l4 4L19 7" />
      </svg>
    ),
    label: '',
  },
  error: {
    dot: 'bg-red-500',
    pulse: false,
    icon: (
      <svg className="w-3 h-3 text-red-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={3} d="M6 18L18 6M6 6l12 12" />
      </svg>
    ),
    label: '',
  },
};

export function ToolCallRow({
  description,
  status,
  filePath,
  operation,
  summary,
  error,
  durationMs,
  onExpand,
  isExpanded,
  className,
  children,
}: ToolCallRowProps) {
  const config = statusConfig[status];

  const displayText = React.useMemo(() => {
    if (status === 'preparing') {
      return `${config.label} ${description}${filePath ? ` ${filePath}` : ''}...`;
    }
    if (status === 'running') {
      return `${config.label} ${description}${filePath ? ` ${filePath}` : ''}...`;
    }
    if (status === 'error' && error) {
      return `${description}${filePath ? ` ${filePath}` : ''}: ${error}`;
    }
    return summary || `${description}${filePath ? ` ${filePath}` : ''}`;
  }, [description, filePath, status, error, summary, config.label]);

  return (
    <div className={cn('group', className)}>
      <motion.div
        initial={{ opacity: 0, x: -4 }}
        animate={{ opacity: 1, x: 0 }}
        transition={{ duration: 0.15, ease: 'easeOut' }}
        className={cn(
          'flex items-center gap-2 py-1 px-0 text-sm',
          status === 'error' && 'text-red-400',
          onExpand && 'cursor-pointer hover:bg-accent/50 rounded-sm'
        )}
        onClick={onExpand}
      >
        <span className="flex-shrink-0 w-4 flex justify-center">
          {config.icon ? (
            config.icon
          ) : (
            <span
              className={cn(
                'w-2 h-2 rounded-full',
                config.dot,
                config.pulse && 'animate-pulse'
              )}
            />
          )}
        </span>

        <span className="text-muted-foreground flex-shrink-0">{description}</span>

        {filePath && (
          <span className={cn(
            'text-foreground font-mono text-xs truncate max-w-[200px]',
            status === 'error' && 'text-red-400'
          )}>
            {filePath}
          </span>
        )}

        {operation && status !== 'error' && (
          <span className="text-muted-foreground text-xs">[{operation}]</span>
        )}

        {summary && status !== 'preparing' && status !== 'running' && status !== 'error' && (
          <span className="text-foreground/80 truncate">{summary}</span>
        )}

        {error && status === 'error' && (
          <span className="text-red-400/90 truncate font-mono text-[11px] ml-1 border-l border-red-500/20 pl-2">
            {error}
          </span>
        )}

        {durationMs && status !== 'preparing' && status !== 'running' && (
          <span className="text-muted-foreground text-xs ml-auto flex-shrink-0">
            {durationMs < 1000 ? `${durationMs}ms` : `${(durationMs / 1000).toFixed(1)}s`}
          </span>
        )}

        {onExpand && (
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
        )}
      </motion.div>

      {isExpanded && children && (
        <motion.div
          initial={{ opacity: 0, height: 0 }}
          animate={{ opacity: 1, height: 'auto' }}
          exit={{ opacity: 0, height: 0 }}
          transition={{ duration: 0.15, ease: 'easeOut' }}
          className="ml-6 pl-2 border-l border-border"
        >
          {children}
        </motion.div>
      )}
    </div>
  );
}
