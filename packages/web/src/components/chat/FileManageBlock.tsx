'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';

export default function FileManageBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result || !result.message) return null;
    
    return (
      <div className="text-xs bg-accent/20 p-2 rounded italic opacity-80">
        {result.message}
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
