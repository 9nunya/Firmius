'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Code, Map, Info, AlertTriangle } from 'lucide-react';
import { cn } from '@/lib/utils';

export default function LSPBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const getSymbolTypeBadge = (kind: string) => {
    const kindColors: Record<string, string> = {
      Class: 'text-purple-400 border-purple-400/50',
      Function: 'text-green-400 border-green-400/50',
      Method: 'text-yellow-400 border-yellow-400/50',
      Interface: 'text-blue-400 border-blue-400/50',
      Enum: 'text-orange-400 border-orange-400/50',
    };
    return kindColors[kind] || 'text-slate-400 border-slate-400/50';
  };

  const renderDetail = (result: any, args: any) => {
    if (!result) return null;
    const op = args?.operation;
    
    return (
      <div className="flex flex-col gap-2">
        {result.locations && (
          <div className="flex flex-col gap-1 max-h-60 overflow-y-auto pr-2">
            {result.locations.map((l: any, i: number) => (
              <div key={i} className="flex flex-col gap-0.5 bg-accent/10 p-2 rounded border border-border/50">
                <span className="text-[10px] font-mono font-bold text-primary/70 truncate">{l.file}:{l.line + 1}</span>
                {l.context && <div className="text-[11px] font-mono opacity-80 truncate">{l.context}</div>}
              </div>
            ))}
          </div>
        )}
        {result.symbols && (
          <div className="grid grid-cols-1 gap-1 max-h-80 overflow-y-auto pr-2">
            {result.symbols.map((s: any, i: number) => (
              <div key={i} className="flex items-center gap-2 text-[11px] bg-accent/20 p-1.5 px-2 rounded group">
                <span className={cn("text-[9px] font-black uppercase px-1 rounded border", getSymbolTypeBadge(s.kind))}>{s.kind}</span>
                <span className="font-mono font-bold">{s.name}</span>
                <span className="ml-auto text-[9px] text-muted-foreground opacity-0 group-hover:opacity-100">L{s.range.start.line + 1}</span>
              </div>
            ))}
          </div>
        )}
        {result.hover && (
          <div className="bg-accent/20 p-3 rounded border border-border/50 max-h-60 overflow-y-auto">
            <pre className="text-[11px] font-mono whitespace-pre-wrap">{typeof result.hover === 'string' ? result.hover : JSON.stringify(result.hover, null, 2)}</pre>
          </div>
        )}
        {result.diagnostics && result.diagnostics.length > 0 && (
           <div className="flex flex-col gap-1">
             {result.diagnostics.map((d: any, i: number) => (
               <div key={i} className={cn("flex items-start gap-2 p-2 rounded text-xs", d.severity === 1 ? "bg-red-500/10 text-red-400" : "bg-amber-500/10 text-amber-400")}>
                 <AlertTriangle size={12} className="mt-0.5 shrink-0" />
                 <div className="flex flex-col">
                   <span className="font-mono text-[10px] font-bold">L{d.range.start.line + 1}</span>
                   <span>{d.message}</span>
                 </div>
               </div>
             ))}
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
