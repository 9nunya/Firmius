'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';

export default function ProcessExecuteBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.stdout && (
          <div className="flex flex-col gap-1">
             <span className="text-[10px] font-bold text-emerald-500/70 uppercase">Stdout</span>
             <div className="bg-black/90 text-emerald-400 p-2 rounded text-[11px] font-mono whitespace-pre-wrap max-h-[300px] overflow-y-auto">
               {result.stdout}
             </div>
          </div>
        )}
        {result.stderr && (
          <div className="flex flex-col gap-1">
             <span className="text-[10px] font-bold text-red-500/70 uppercase">Stderr</span>
             <div className="bg-black/90 text-red-400 p-2 rounded text-[11px] font-mono whitespace-pre-wrap max-h-[200px] overflow-y-auto">
               {result.stderr}
             </div>
          </div>
        )}
        {result.exitCode !== undefined && (
          <div className="text-[10px] font-bold text-muted-foreground uppercase tracking-tight">
            Exit Code: <span className={result.exitCode === 0 ? "text-emerald-500" : "text-red-500"}>{result.exitCode}</span>
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
