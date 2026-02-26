'use client';

import React, { useState } from 'react';
import { ToolCallRow, type ToolStatus } from './ToolCallRow';
import { cn } from '@/lib/utils';
import type { Message } from '../../types';

export interface BaseToolBlockProps {
  toolCall: NonNullable<Message['toolCalls']>[number];
  renderDetail?: (result: any, args: any, metadata?: Record<string, any>) => React.ReactNode;
  renderError?: (error: string, result: any, args: any) => React.ReactNode;
  className?: string;
}

export function BaseToolBlock({
  toolCall,
  renderDetail,
  renderError,
  className,
}: BaseToolBlockProps) {
  const [isExpanded, setIsExpanded] = React.useState(false);

  const { name, status, args, result, summary, error, durationMs, metadata } = toolCall as any;

  // Auto-expand when running and we get metadata (like spawnedAgentId)
  React.useEffect(() => {
    if (status === 'running' && metadata && Object.keys(metadata).length > 0) {
      setIsExpanded(true);
    }
  }, [status, metadata]);

  const canExpand = (status === 'done' && !!renderDetail) || 
                    (status === 'error' && (!!error || !!renderError || !!result)) ||
                    (status === 'running' && !!metadata);

  return (
    <ToolCallRow
      description={name}
      status={status}
      summary={summary}
      error={error}
      durationMs={durationMs}
      onExpand={canExpand ? () => setIsExpanded(!isExpanded) : undefined}
      isExpanded={isExpanded}
      className={className}
    >
      <div className="py-2">
        {((status === 'done' && result) || (status === 'running' && !!metadata)) && 
          renderDetail && (renderDetail(result, args, metadata) as React.ReactNode)}
        {status === 'error' && (renderError ? (renderError(error || '', result, args) as React.ReactNode) : (
            <div className="text-red-400 text-xs font-mono whitespace-pre-wrap">
              {error || (result && typeof result === 'string' ? result : 'An unknown error occurred.')}
            </div>
        ))}
      </div>
    </ToolCallRow>
  );
}
