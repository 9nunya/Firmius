'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Activity, Brain, Cpu } from 'lucide-react';

export default function ContextManageBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.metrics && (
          <div className="grid grid-cols-2 gap-2">
            <div className="bg-accent/20 p-2 rounded flex flex-col">
              <span className="text-[9px] text-muted-foreground uppercase flex items-center gap-1">
                <Brain size={8} /> Total Tokens
              </span>
              <span className="text-xs font-mono font-bold">{result.metrics.totalTokens.toLocaleString()}</span>
            </div>
            <div className="bg-accent/20 p-2 rounded flex flex-col">
              <span className="text-[9px] text-muted-foreground uppercase flex items-center gap-1">
                <Cpu size={8} /> Last Turn
              </span>
              <span className="text-xs font-mono font-bold">{result.metrics.lastTurnTokens.toLocaleString()}</span>
            </div>
          </div>
        )}
        {result.activeSubAgents && result.activeSubAgents.length > 0 && (
          <div className="flex flex-col gap-1">
            <span className="text-[10px] font-bold text-muted-foreground uppercase flex items-center gap-1">
              <Activity size={10} /> Active Sub-agents
            </span>
            <div className="flex flex-col gap-1">
              {result.activeSubAgents.map((a: any) => (
                <div key={a.id} className="text-xs bg-accent/10 p-1 px-2 rounded border border-border/50 truncate">
                  <span className="font-mono text-[10px] mr-2">{a.id}</span>
                  <span className="opacity-70">{a.purpose}</span>
                </div>
              ))}
            </div>
          </div>
        )}
        {result.message && (
          <div className="text-xs italic text-muted-foreground bg-accent/10 p-2 rounded">
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
