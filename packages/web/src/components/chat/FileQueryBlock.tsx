'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import { Folder, File, Search } from 'lucide-react';

export default function FileQueryBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-2">
        {result.entries && (
          <div className="grid grid-cols-1 gap-1 max-h-[300px] overflow-y-auto pr-2">
            {result.entries.map((e: any, i: number) => (
              <div key={i} className="flex items-center gap-2 text-xs hover:bg-accent/50 p-1 rounded group">
                {e.isDirectory ? <Folder size={12} className="text-blue-400" /> : <File size={12} className="text-muted-foreground" />}
                <span className="font-mono truncate">{e.name}</span>
                {!e.isDirectory && <span className="ml-auto text-[10px] text-muted-foreground opacity-0 group-hover:opacity-100">{Math.round(e.size / 1024)} KB</span>}
              </div>
            ))}
          </div>
        )}
        {result.matches && (
          <div className="flex flex-col gap-2 max-h-[400px] overflow-y-auto pr-2">
             {result.matches.map((m: any, i: number) => (
               <div key={i} className="flex flex-col gap-1 bg-accent/20 p-2 rounded">
                 <div className="flex items-center gap-2">
                    <Search size={10} className="text-muted-foreground" />
                    <span className="text-[10px] font-mono font-bold text-primary/80">{m.file}:{m.line}</span>
                 </div>
                 <div className="text-[11px] font-mono whitespace-pre overflow-x-auto opacity-90 border-l border-primary/20 pl-2">
                   {m.content}
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
