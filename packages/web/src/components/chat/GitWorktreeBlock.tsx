'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { GitBranch, List } from 'lucide-react';

export default function GitWorktreeBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.worktrees && (
          <div className="flex flex-col gap-1 max-h-48 overflow-y-auto pr-2">
             <span className="text-[10px] font-bold text-muted-foreground uppercase flex items-center gap-1">
               <List size={10} /> Active Worktrees
             </span>
             {result.worktrees.map((wt: any, i: number) => (
               <div key={i} className="flex items-center gap-2 bg-accent/10 p-1.5 px-2 rounded text-[11px]">
                 <GitBranch size={10} className="text-primary/70" />
                 <span className="font-mono font-bold">{wt.name}</span>
                 <span className="ml-auto opacity-70 italic">{wt.branch}</span>
               </div>
             ))}
          </div>
        )}
        {result.message && (
          <div className="text-xs bg-accent/10 p-2 rounded italic opacity-70">
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
