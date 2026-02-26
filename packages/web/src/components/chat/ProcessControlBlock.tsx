'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Terminal, Activity, List, X } from 'lucide-react';
import { cn } from '@/lib/utils';

export default function ProcessControlBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any, args: any) => {
    if (!result) return null;
    const op = args?.operation;
    
    return (
      <div className="flex flex-col gap-2">
        {result.pid && (
          <div className="flex items-center gap-2 bg-accent/20 p-2 rounded">
             <Terminal size={12} className="text-primary" />
             <span className="text-xs font-mono">PID: {result.pid}</span>
             <span className="ml-auto text-[9px] font-black uppercase text-green-500 animate-pulse">Running</span>
          </div>
        )}
        {result.processes && (
          <div className="flex flex-col gap-1 max-h-48 overflow-y-auto pr-2">
             <span className="text-[10px] font-bold text-muted-foreground uppercase flex items-center gap-1">
               <List size={10} /> Managed Processes
             </span>
             {result.processes.map((p: any) => (
               <div key={p.pid} className="flex items-center gap-2 bg-accent/10 p-1.5 px-2 rounded text-[11px]">
                 <span className="font-mono text-[10px] bg-accent/30 px-1 rounded">#{p.pid}</span>
                 <span className={cn("ml-auto text-[9px] uppercase font-bold", p.completed ? "text-muted-foreground" : "text-primary")}>
                   {p.completed ? "Completed" : "Active"}
                 </span>
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
