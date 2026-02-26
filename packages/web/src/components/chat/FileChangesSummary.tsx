'use client';

import React from 'react';
import { motion } from 'framer-motion';
import { cn } from '@/lib/utils';
import { useFileChanges } from '@/hooks/useFileChanges';

interface FileChangesSummaryProps {
  threadId: string;
  agentId: string;
  turnIndex?: number;
  className?: string;
}

export function FileChangesSummary({
  threadId,
  agentId,
  turnIndex,
  className,
}: FileChangesSummaryProps) {
  const { summary, isLoading } = useFileChanges({
    threadId,
    agentId,
    turnIndex,
    enabled: false, // No polling - static summary only
  });

  if (isLoading || !summary || (summary.additions === 0 && summary.deletions === 0)) {
    return null;
  }

  return (
    <span className={cn('ml-2 text-xs font-mono', className)}>
      <span className="text-green-500">+{summary.additions}</span>
      <span className="text-red-500 ml-1">-{summary.deletions}</span>
      {summary.totalFiles > 0 && (
        <span className="text-muted-foreground ml-1">
          ({summary.totalFiles} file{summary.totalFiles !== 1 ? 's' : ''})
        </span>
      )}
    </span>
  );
}
