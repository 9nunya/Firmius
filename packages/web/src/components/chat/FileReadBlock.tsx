'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';

export default function FileReadBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result || !result.content) return null;
    
    return (
      <div className="flex flex-col gap-1">
        <div className="bg-accent/30 rounded p-2 text-xs font-mono overflow-x-auto max-h-[300px] overflow-y-auto whitespace-pre">
          {result.content}
        </div>
        <div className="text-[10px] text-muted-foreground text-right italic">
          Total lines: {result.totalLines}
        </div>
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
