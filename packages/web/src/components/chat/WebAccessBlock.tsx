'use client';

import React from 'react';
import { BaseToolBlock } from './BaseToolBlock';
import type { Message } from '../../types';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { ExternalLink, Globe, FileText } from 'lucide-react';

export default function WebAccessBlock({ toolCall }: { toolCall: NonNullable<Message['toolCalls']>[number] }) {
  const renderDetail = (result: any) => {
    if (!result) return null;
    
    return (
      <div className="flex flex-col gap-3">
        {result.operation === 'search' && (
          <div className="flex flex-col gap-2">
            <div className="prose prose-sm dark:prose-invert max-w-none bg-accent/20 p-3 rounded text-xs leading-relaxed">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{result.result}</ReactMarkdown>
            </div>
            {result.citations && result.citations.length > 0 && (
              <div className="flex flex-col gap-1">
                <span className="text-[10px] font-bold text-muted-foreground uppercase">Sources</span>
                <div className="flex flex-wrap gap-2">
                  {result.citations.map((c: string, i: number) => (
                    <a key={i} href={c} target="_blank" rel="noopener noreferrer" className="flex items-center gap-1 text-[10px] bg-primary/10 hover:bg-primary/20 text-primary p-1 px-2 rounded transition-colors max-w-[200px]">
                      <Globe size={10} />
                      <span className="truncate">{new URL(c).hostname}</span>
                      <ExternalLink size={8} />
                    </a>
                  ))}
                </div>
              </div>
            )}
          </div>
        )}
        {result.operation === 'fetch' && (
          <div className="flex flex-col gap-2">
            <span className="text-[10px] font-bold text-muted-foreground uppercase flex items-center gap-1">
              <FileText size={10} />
              Markdown Preview
            </span>
            <div className="prose prose-sm dark:prose-invert max-w-none bg-accent/10 p-3 rounded text-xs max-h-[400px] overflow-y-auto">
              <ReactMarkdown remarkPlugins={[remarkGfm]}>{result.content}</ReactMarkdown>
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
