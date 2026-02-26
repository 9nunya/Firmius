'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Bot, User, ArrowRight } from 'lucide-react';
import { cn } from '@/lib/utils';
import useAppStore from '../../stores/app-store';

export default function SubagentToolBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const focusAgent = useAppStore(s => s.focusAgent);

  const renderDetail = (result: any, args: any, metadata?: Record<string, any>) => {
    const spawnedId = metadata?.spawnedAgentId || (toolCall as any).spawnedAgentId;
    
    return (
      <div className="flex flex-col gap-3">
        {spawnedId && (
          <div className="bg-primary/5 border border-primary/20 rounded-md p-3 flex items-center gap-3">
            <Bot className="text-primary w-5 h-5 shrink-0" />
            <div className="flex-1 min-w-0">
              <div className="text-[10px] font-bold text-primary uppercase leading-none mb-1">Active Sub-agent</div>
              <div className="text-xs font-mono opacity-80 truncate">{spawnedId}</div>
            </div>
            <button 
              onClick={() => focusAgent(spawnedId)}
              className="flex items-center gap-1.5 px-2 py-1 bg-primary/10 hover:bg-primary/20 text-primary rounded text-[11px] font-bold transition-colors whitespace-nowrap"
            >
              Focus <ArrowRight size={10} />
            </button>
          </div>
        )}

        {result?.result && (
          <div className="flex flex-col gap-1">
             <span className="text-[10px] font-bold text-muted-foreground uppercase tracking-tight">Final Response</span>
             <div className="bg-accent/20 rounded p-3 text-sm italic relative pl-8">
               <Bot className="absolute left-2 top-3 w-4 h-4 text-primary opacity-50" />
               {result.result}
             </div>
          </div>
        )}
        {result?.agent && (
          <div className="grid grid-cols-2 gap-2 p-2 bg-accent/10 rounded">
            <div className="flex flex-col">
              <span className="text-[9px] text-muted-foreground uppercase">Status</span>
              <span className="text-xs font-medium uppercase">{result.agent.status}</span>
            </div>
            <div className="flex flex-col">
              <span className="text-[9px] text-muted-foreground uppercase">Objective</span>
              <span className="text-xs truncate">{result.agent.objective}</span>
            </div>
          </div>
        )}
        {result?.agents && Array.isArray(result.agents) && (
          <div className="flex flex-col gap-1">
            <span className="text-[10px] font-bold text-muted-foreground uppercase">Child Agents</span>
            <div className="flex flex-col gap-1">
              {result.agents.map((a: any) => (
                <div key={a.id} className="flex items-center gap-2 text-xs bg-accent/20 p-1 px-2 rounded">
                  <div className={cn("w-2 h-2 rounded-full", a.status === 'working' ? "bg-amber-500 animate-pulse" : "bg-green-500")} />
                  <span className="font-mono text-[10px]">{a.id}</span>
                  <span className="ml-auto text-muted-foreground text-[10px] uppercase">{a.status}</span>
                </div>
              ))}
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
