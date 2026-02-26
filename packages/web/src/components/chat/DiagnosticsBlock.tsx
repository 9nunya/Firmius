'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { AlertTriangle, Info, XCircle } from 'lucide-react';
import { cn } from '@/lib/utils';

export default function DiagnosticsBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const getSeverityInfo = (severity: number) => {
    switch (severity) {
      case 1: return { icon: XCircle, color: 'text-red-500' };
      case 2: return { icon: AlertTriangle, color: 'text-yellow-500' };
      default: return { icon: Info, color: 'text-blue-500' };
    }
  };

  const renderDetail = (result: any) => {
    const diagnostics = result?.diagnostics || [];
    if (diagnostics.length === 0) return <div className="text-xs text-muted-foreground italic">No diagnostics found.</div>;
    
    return (
      <div className="flex flex-col gap-1.5 max-h-80 overflow-y-auto pr-2">
        {diagnostics.map((d: any, i: number) => {
          const { icon: Icon, color } = getSeverityInfo(d.severity);
          return (
            <div key={i} className="flex items-start gap-2 bg-accent/20 p-2 rounded">
              <Icon size={12} className={cn("mt-0.5 shrink-0", color)} />
              <div className="flex flex-col">
                <span className="text-[10px] font-mono font-bold opacity-70">L{d.range?.start?.line + 1 || d.line}</span>
                <span className="text-xs">{d.message}</span>
              </div>
            </div>
          );
        })}
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
