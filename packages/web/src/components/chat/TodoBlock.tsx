'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { CheckCircle2, Circle, Clock } from 'lucide-react';
import { cn } from '@/lib/utils';

export default function TodoBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const getStatusIcon = (status: string) => {
    switch (status) {
      case 'completed': return <CheckCircle2 size={12} className="text-green-500" />;
      case 'in_progress': return <Clock size={12} className="text-blue-500 animate-pulse" />;
      default: return <Circle size={12} className="text-muted-foreground/50" />;
    }
  };

  const getPriorityColor = (priority: string) => {
    switch (priority) {
      case 'high': return 'text-red-400';
      case 'medium': return 'text-amber-400';
      default: return 'text-slate-400';
    }
  };

  const renderDetail = (result: any) => {
    if (!result || !result.todos) return null;
    
    return (
      <div className="flex flex-col gap-1.5 max-h-80 overflow-y-auto pr-2">
        {result.todos.map((t: any) => (
          <div key={t.id} className="flex items-center gap-2 bg-accent/20 p-2 rounded group">
            {getStatusIcon(t.status)}
            <span className={cn("text-xs flex-1 truncate", t.status === 'completed' && "line-through opacity-50")}>
              {t.content}
            </span>
            <span className={cn("text-[8px] font-black uppercase tracking-tighter px-1 rounded border border-current opacity-50", getPriorityColor(t.priority))}>
              {t.priority}
            </span>
          </div>
        ))}
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
