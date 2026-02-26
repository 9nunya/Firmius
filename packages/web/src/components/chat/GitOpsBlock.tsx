'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { GitCommit, GitBranch, History } from 'lucide-react';

export default function GitOpsBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any, args: any) => {
    if (!result || !result.output) return null;
    
    const operation = args?.operation;

    if (operation === 'log') {
      const commits = result.output.split('\n').filter((l: string) => l.trim());
      return (
        <div className="flex flex-col gap-1 max-h-64 overflow-y-auto pr-2">
          {commits.map((c: string, i: number) => {
            const [sha, ...msgParts] = c.split(' ');
            return (
              <div key={i} className="flex gap-2 text-xs bg-accent/20 p-2 rounded hover:bg-accent/30 transition-colors">
                <span className="font-mono text-primary font-bold">{sha}</span>
                <span className="text-muted-foreground truncate">{msgParts.join(' ')}</span>
              </div>
            );
          })}
        </div>
      );
    }

    if (operation === 'status' || operation === 'diff' || operation === 'stash') {
       return (
         <div className="bg-black/90 p-3 rounded border border-border/50">
           <pre className="text-[11px] font-mono text-foreground whitespace-pre-wrap max-h-[400px] overflow-y-auto">
             {result.output}
           </pre>
         </div>
       );
    }

    return (
      <div className="text-xs bg-accent/20 p-2 rounded italic">
        {result.output}
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
