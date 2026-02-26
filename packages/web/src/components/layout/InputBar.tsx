'use client';

import React, { useState, useRef, useEffect, useMemo } from 'react';
import useAppStore, { selectWorkingAgentId } from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { X, Paperclip, Square, Send } from 'lucide-react';

interface AttachedFile {
  id: string;
  name: string;
  type: string;
  data: string; // Base64
  previewUrl?: string;
}

interface InputBarProps {
  className?: string;
  isMobile?: boolean;
}

export function InputBar({ className, isMobile }: InputBarProps): React.ReactElement {
  const [inputValue, setInputValue] = useState('');
  const [attachedFiles, setAttachedFiles] = useState<AttachedFile[]>([]);
  const textareaRef = useRef<HTMLTextAreaElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const { activeThreadId, sendMessage, interruptThread, agents, activeAgentId } = useAppStore();
  const workingAgentId = useAppStore(selectWorkingAgentId);
  const isGenerating = workingAgentId !== null;

  const isViewingSubagent = useMemo(() => {
    if (!activeAgentId) return false;
    const agent = agents.find(a => a.id === activeAgentId);
    return agent && !agent.isLead;
  }, [activeAgentId, agents]);

  const isSubagentDisabled = isViewingSubagent;

  const handlePaste = async (e: React.ClipboardEvent) => {
    const items = e.clipboardData.items;
    for (const item of Array.from(items)) {
      if (item.type.startsWith('image/')) {
        const file = item.getAsFile();
        if (file) {
          await processFile(file);
        }
      }
    }
  };

  const handleDrop = async (e: React.DragEvent) => {
    e.preventDefault();
    const files = Array.from(e.dataTransfer.files);
    for (const file of files) {
      await processFile(file);
    }
  };

  const processFile = async (file: File) => {
    return new Promise<void>((resolve) => {
      const reader = new FileReader();
      reader.onload = (e) => {
        const data = e.target?.result as string;
        const attached: AttachedFile = {
          id: Math.random().toString(36).slice(2, 9),
          name: file.name,
          type: file.type,
          data: data,
          previewUrl: file.type.startsWith('image/') ? data : undefined,
        };
        setAttachedFiles(prev => [...prev, attached]);
        resolve();
      };
      reader.readAsDataURL(file);
    });
  };

  const removeFile = (id: string) => {
    setAttachedFiles(prev => prev.filter(f => f.id !== id));
  };

  // Auto-resize textarea
  useEffect(() => {
    if (textareaRef.current) {
      textareaRef.current.style.height = 'auto';
      textareaRef.current.style.height = `${Math.min(textareaRef.current.scrollHeight, 200)}px`;
    }
  }, [inputValue]);

  const handleKeyDown = (e: React.KeyboardEvent<HTMLTextAreaElement>) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  const handleSend = () => {
    const trimmedValue = inputValue.trim();
    if ((!trimmedValue && attachedFiles.length === 0) || !activeThreadId) {
      return;
    }

    let messageContent: string | unknown[] = trimmedValue;

    if (attachedFiles.length > 0) {
      messageContent = [
        { type: 'text', text: trimmedValue || ' ' },
        ...attachedFiles.map(file => ({
          type: 'image_url',
          image_url: { url: file.data }
        }))
      ];
    }

    void sendMessage(messageContent);
    setInputValue('');
    setAttachedFiles([]);
  };

  const isDisabled = !activeThreadId || isSubagentDisabled || (inputValue.trim().length === 0 && attachedFiles.length === 0);

  return (
    <div
      className={cn(
        'border-t border-border bg-card',
        className
      )}
      onDragOver={(e) => e.preventDefault()}
      onDrop={handleDrop}
    >
      <div className="space-y-2">
        {/* File Preview Row */}
        {attachedFiles.length > 0 && (
          <div className="flex flex-wrap gap-2 px-4 pt-2">
            {attachedFiles.map((file) => (
              <div
                key={file.id}
                className="relative group w-16 h-16 bg-muted border border-border flex items-center justify-center"
              >
                {file.previewUrl ? (
                  /* eslint-disable-next-line @next/next/no-img-element */
                  <img src={file.previewUrl} alt={file.name} className="w-full h-full object-cover" />
                ) : (
                  <Paperclip size={20} className="text-muted-foreground" />
                )}
                <button
                  onClick={() => removeFile(file.id)}
                  className="absolute -top-1 -right-1 bg-background text-foreground p-0.5 opacity-0 group-hover:opacity-100 transition-opacity"
                >
                  <X size={12} />
                </button>
                <div className="absolute bottom-0 left-0 right-0 bg-background/90 px-1 py-0.5 text-[8px] truncate text-muted-foreground">
                  {file.name}
                </div>
              </div>
            ))}
          </div>
        )}

        <div className="flex items-stretch h-14">
          <div className="flex-1 relative flex">
            <textarea
              ref={textareaRef}
              value={inputValue}
              onChange={(e) => setInputValue(e.target.value)}
              onKeyDown={handleKeyDown}
              onPaste={handlePaste}
              placeholder={isSubagentDisabled ? "Viewing subagent - select lead to send messages" : "Message..."}
              disabled={!activeThreadId || isSubagentDisabled}
              className={cn(
                'h-full w-full resize-none border-0 border-r border-border bg-background px-4 py-3 pr-12 text-sm transition-colors',
                'placeholder:text-muted-foreground focus:outline-none',
                'disabled:cursor-not-allowed disabled:opacity-50',
                'scrollbar-hide'
              )}
              rows={1}
            />
            <div className="absolute right-2 top-1/2 -translate-y-1/2 flex items-center">
              <button
                type="button"
                onClick={() => fileInputRef.current?.click()}
                disabled={!activeThreadId}
                className="p-2 text-muted-foreground hover:text-foreground transition-colors"
                title="Attach file"
              >
                <Paperclip size={18} />
              </button>
            </div>
            <input
              type="file"
              ref={fileInputRef}
              className="hidden"
              multiple
              onChange={(e) => {
                const files = Array.from(e.target.files || []);
                files.forEach(processFile);
              }}
            />
          </div>

          <button
            type="button"
            onClick={isGenerating ? interruptThread : handleSend}
            disabled={!activeThreadId || (!isGenerating && isDisabled)}
            className={cn(
              'w-14 h-full shrink-0 flex items-center justify-center transition-colors',
              (!activeThreadId || (!isGenerating && isDisabled))
                ? 'bg-muted text-muted-foreground cursor-not-allowed opacity-50'
                : isGenerating
                  ? 'bg-destructive text-destructive-foreground hover:bg-destructive/90'
                  : 'bg-primary text-primary-foreground hover:bg-primary/90'
            )}
            title={isGenerating ? `Stop ${agents.find(a => a.id === workingAgentId)?.readableName || 'generation'}` : "Send message"}
          >
            {isGenerating ? (
              <Square size={18} fill="currentColor" />
            ) : (
              <Send size={20} className={isDisabled ? 'opacity-50' : ''} />
            )}
          </button>
        </div>
      </div>
    </div>
  );
}

export default InputBar;
