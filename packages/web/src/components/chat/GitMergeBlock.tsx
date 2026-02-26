'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { AlertCircle, CheckCircle2, GitBranch } from 'lucide-react';

export default function GitMergeBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.conflictFiles && result.conflictFiles.length > 0 && (
          <div className="flex flex-col gap-1">
            <span className="text-[10px] font-bold text-red-400 uppercase flex items-center gap-1">
              <AlertCircle size={10} /> Merge Conflicts
            </span>
            <div className="flex flex-col gap-1">
              {result.conflictFiles.map((f: string, i: number) => (
                <div key={i} className="text-xs bg-red-500/10 text-red-400 p-1 px-2 rounded font-mono">
                  {f}
                </div>
              ))}
            </div>
          </div>
        )}
        {result.filesChanged !== undefined && (
          <div className="text-xs text-muted-foreground">
            Files changed: <span className="text-foreground font-bold">{result.filesChanged}</span>
          </div>
        )}
        {result.message && (
          <div className="text-xs italic opacity-70">
            {result.message}
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
