'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Zap, List, Scissors } from 'lucide-react';

export default function CompactionManageBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.turns && (
          <div className="flex flex-col gap-1 max-h-60 overflow-y-auto pr-2">
            <span className="text-[10px] font-bold text-muted-foreground uppercase flex items-center gap-1">
              <List size={10} /> History turns
            </span>
            {result.turns.map((t: any) => (
              <div key={t.index} className="flex items-center gap-2 bg-accent/10 p-1.5 px-2 rounded text-[11px]">
                <span className="font-mono text-[10px] bg-accent/30 px-1 rounded">#{t.index}</span>
                <span className="capitalize opacity-70">{t.role}</span>
                <span className="ml-auto font-mono text-[10px] opacity-50">{t.tokens} tokens</span>
                {t.summaryAvailable && <Zap size={10} className="text-amber-500" />}
              </div>
            ))}
          </div>
        )}
        {result.details && (
          <div className="flex flex-col gap-2 max-h-80 overflow-y-auto pr-2">
             {result.details.map((d: any) => (
               <div key={d.index} className="flex flex-col gap-1 bg-accent/20 p-2 rounded border border-border/50">
                 <span className="text-[10px] font-bold text-primary/70">Turn #{d.index}</span>
                 <div className="text-[11px] font-mono whitespace-pre-wrap opacity-80">
                   {typeof d.content === 'string' ? d.content : JSON.stringify(d.content, null, 2)}
                 </div>
               </div>
             ))}
          </div>
        )}
        {result.message && (
          <div className="flex items-center gap-2 text-xs bg-green-500/10 text-green-400 p-2 rounded">
             <Scissors size={12} />
             <span>{result.message}</span>
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
