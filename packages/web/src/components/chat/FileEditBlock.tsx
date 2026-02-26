'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import { MarkdownContent } from './MarkdownContent';
import type { Message } from '../../types';

export default function FileEditBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any, args: any) => {
    // Robustly handle args which might be a JSON string or an object
    const getArg = (key: string) => {
      if (!args) return undefined;
      if (typeof args === 'object') return args[key];
      try {
        const parsed = JSON.parse(args);
        return parsed[key];
      } catch {
        return undefined;
      }
    };

    const operation = getArg('operation');
    const content = getArg('content');
    
    return (
      <div className="flex flex-col gap-2">
        {operation === 'write' && content && (
          <div className="flex flex-col gap-1">
            <span className="text-[10px] font-bold text-muted-foreground uppercase tracking-tight">Written Content</span>
            <div className="bg-accent/30 rounded p-2 text-xs font-mono max-h-[300px] overflow-auto whitespace-pre">
              {content}
            </div>
          </div>
        )}
        {result?.diagnostics && result.diagnostics.length > 0 && (
          <div className="flex flex-col gap-1 border-l-2 border-amber-500/50 pl-2">
            <span className="text-[10px] font-bold text-amber-500 uppercase">LSP Diagnostics</span>
            {result.diagnostics.map((d: any, i: number) => (
              <div key={i} className="text-xs text-foreground/70 font-mono">
                L{d.range.start.line + 1}: {d.message}
              </div>
            ))}
          </div>
        )}
        {result?.matchedContent && (
          <div className="flex flex-col gap-1">
            <span className="text-[10px] font-bold text-muted-foreground uppercase tracking-tight">Context matched</span>
            <div className="bg-accent/30 rounded p-2 text-xs font-mono opacity-80 overflow-x-auto">
              {result.matchedContent}
            </div>
          </div>
        )}
      </div>
    );
  };

  return (
    <BaseToolBlock 
      toolCall={toolCall} 
      renderDetail={renderDetail}
    />
  );
}
