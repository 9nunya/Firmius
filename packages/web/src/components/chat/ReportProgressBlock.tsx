'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Activity } from 'lucide-react';

export default function ReportProgressBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result || !result.message) return null;
    
    return (
      <div className="flex items-center gap-2 text-xs bg-blue-500/10 text-blue-400 p-2 rounded animate-pulse">
        <Activity size={12} />
        <span>{result.message}</span>
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
