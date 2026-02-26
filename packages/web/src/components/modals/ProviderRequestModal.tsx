'use client';

import React, { useState, useEffect } from 'react';
import { X, Copy, Check, Terminal, ChevronDown, ChevronRight } from 'lucide-react';
import { motion, AnimatePresence } from 'framer-motion';
import { cn } from '@/lib/utils';

interface ProviderRequestModalProps {
  isOpen: boolean;
  onClose: () => void;
  request: Record<string, unknown>;
}

interface MessagePart {
  type: string;
  role?: string;
  content?: string;
  text?: string;
  tool_calls?: unknown[];
  tool_call_id?: string;
  name?: string;
}

function MessageContent({ content }: { content: string | MessagePart[] | unknown }) {
  if (typeof content === 'string') {
    return <pre className="whitespace-pre-wrap text-sm overflow-x-auto max-w-full">{content}</pre>;
  }

  if (!Array.isArray(content)) {
    return <pre className="whitespace-pre-wrap text-sm overflow-x-auto max-w-full">{JSON.stringify(content, null, 2)}</pre>;
  }

  return (
    <div className="flex flex-col gap-2">
      {content.map((part, i) => {
        const p = part as MessagePart;
        if (p.type === 'text' && p.content) {
          return <div key={i} className="text-sm whitespace-pre-wrap overflow-x-auto max-w-full">{p.content}</div>;
        }
        if (p.type === 'tool_use') {
          return (
            <div key={i} className="bg-purple-500/10 border border-purple-500/30 rounded p-2 overflow-x-auto">
              <div className="text-xs font-bold text-purple-400 mb-1">Tool: {p.name}</div>
              <pre className="text-xs text-purple-300 overflow-x-auto max-w-full">
                {JSON.stringify((p as any).input, null, 2)}
              </pre>
            </div>
          );
        }
        return null;
      })}
    </div>
  );
}

export function ProviderRequestModal({ isOpen, onClose, request }: ProviderRequestModalProps) {
  const [copied, setCopied] = useState(false);
  const [expandedRequest, setExpandedRequest] = useState(true);
  const [expandedMessages, setExpandedMessages] = useState(true);
  const [expandedMessageIndices, setExpandedMessageIndices] = useState<Set<number>>(new Set());

  const messages = request.messages as unknown[] | undefined;
  const requestWithoutMessages = Object.entries(request).filter(([key]) => key !== 'messages');

  const toggleMessage = (index: number) => {
    const newExpanded = new Set(expandedMessageIndices);
    if (newExpanded.has(index)) {
      newExpanded.delete(index);
    } else {
      newExpanded.add(index);
    }
    setExpandedMessageIndices(newExpanded);
  };

  const copyToClipboard = async () => {
    await navigator.clipboard.writeText(JSON.stringify(request, null, 2));
    setCopied(true);
    setTimeout(() => setCopied(false), 2000);
  };

  useEffect(() => {
    const handleEsc = (e: KeyboardEvent) => {
      if (e.key === 'Escape') onClose();
    };
    if (isOpen) {
      document.addEventListener('keydown', handleEsc);
    }
    return () => document.removeEventListener('keydown', handleEsc);
  }, [isOpen, onClose]);

  if (!isOpen) return null;

  const isMobile = typeof window !== 'undefined' && window.innerWidth < 768;
  const model = request.model as string | undefined;

  return (
    <AnimatePresence>
      <motion.div
        initial={{ opacity: 0 }}
        animate={{ opacity: 1 }}
        exit={{ opacity: 0 }}
        className={cn(
          "fixed z-50 flex bg-black/60 backdrop-blur-sm p-4",
          isMobile ? "inset-0 items-end md:items-center justify-center" : "inset-0 items-center justify-center"
        )}
        onClick={onClose}
      >
        <motion.div
          initial={{ scale: 0.95, opacity: 0, y: isMobile ? 100 : 0 }}
          animate={{ scale: 1, opacity: 1, y: 0 }}
          exit={{ scale: 0.95, opacity: 0, y: isMobile ? 100 : 0 }}
          className={cn(
            "bg-background border border-border shadow-2xl flex flex-col",
            isMobile ? "w-full rounded-t-2xl md:rounded-xl max-h-[90vh] md:max-h-[85vh]" : "w-full max-w-4xl rounded-xl"
          )}
          onClick={(e) => e.stopPropagation()}
        >
          {/* Header */}
          <div className={cn(
            "flex items-center justify-between border-b border-border",
            isMobile ? "px-4 py-3" : "px-6 py-4"
          )}>
            <div className="flex items-center gap-3">
              <div className={cn("bg-primary/10 rounded-lg", isMobile ? "p-1.5" : "p-2")}>
                <Terminal size={isMobile ? 16 : 20} className="text-primary" />
              </div>
              <div>
                <h2 className={cn("font-semibold", isMobile ? "text-base" : "text-lg")}>Provider Request</h2>
                <p className="text-xs text-muted-foreground">Model: {model || 'unknown'}</p>
              </div>
            </div>
            <div className="flex items-center gap-1">
              <button
                onClick={copyToClipboard}
                className="p-2 hover:bg-accent rounded-lg transition-colors"
                title="Copy to clipboard"
              >
                {copied ? <Check size={isMobile ? 16 : 18} className="text-green-500" /> : <Copy size={isMobile ? 16 : 18} />}
              </button>
              <button
                onClick={onClose}
                className="p-2 hover:bg-accent rounded-lg transition-colors"
              >
                <X size={isMobile ? 16 : 18} />
              </button>
            </div>
          </div>

          {/* Content */}
          <div className={cn("overflow-y-auto", isMobile ? "h-[70vh] px-3 py-2" : "h-[600px] p-4")}>
            <div className="space-y-2 md:space-y-3">
              
              {/* Request Body (excluding messages) */}
              <div className="border border-border rounded-lg overflow-hidden">
                <button
                  onClick={() => setExpandedRequest(!expandedRequest)}
                  className="w-full flex items-center gap-2 md:gap-3 px-3 md:px-4 py-2 md:py-3 bg-muted/30 hover:bg-muted/50 transition-colors text-left"
                >
                  {expandedRequest ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                  <span className="font-mono text-[10px] md:text-xs font-bold px-1.5 md:px-2 py-0.5 rounded bg-blue-500/20 text-blue-400">
                    Request Body
                  </span>
                  <span className="text-xs md:text-sm text-muted-foreground truncate flex-1">
                    {requestWithoutMessages.map(([k, v]) => `${k}: ${typeof v === 'object' ? '{...}' : v}`).join(', ')}
                  </span>
                </button>
                <AnimatePresence>
                  {expandedRequest && (
                    <motion.div
                      initial={{ height: 0 }}
                      animate={{ height: 'auto' }}
                      exit={{ height: 0 }}
                      className="overflow-hidden"
                    >
                      <div className="p-3 md:p-4 border-t border-border bg-background overflow-y-auto max-h-[30vh]">
                        {requestWithoutMessages.map(([key, value]) => (
                          <div key={key} className="mb-3 pb-3 border-b border-border last:border-0">
                            <div className="text-[10px] font-bold text-blue-400 uppercase tracking-wider mb-1">{key}</div>
                            <pre className="text-xs text-foreground whitespace-pre-wrap overflow-x-auto max-w-full">
                              {JSON.stringify(value, null, 2)}
                            </pre>
                          </div>
                        ))}
                      </div>
                    </motion.div>
                  )}
                </AnimatePresence>
              </div>

              {/* Messages */}
              {messages && messages.length > 0 && (
                <div className="border border-border rounded-lg overflow-hidden">
                  <button
                    onClick={() => setExpandedMessages(!expandedMessages)}
                    className="w-full flex items-center gap-2 md:gap-3 px-3 md:px-4 py-2 md:py-3 bg-muted/30 hover:bg-muted/50 transition-colors text-left"
                  >
                    {expandedMessages ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                    <span className="font-mono text-[10px] md:text-xs font-bold px-1.5 md:px-2 py-0.5 rounded bg-green-500/20 text-green-400">
                      Messages
                    </span>
                    <span className="text-xs md:text-sm text-muted-foreground truncate flex-1">
                      {messages.length} messages
                    </span>
                  </button>
                  <AnimatePresence>
                    {expandedMessages && (
                      <motion.div
                        initial={{ height: 0 }}
                        animate={{ height: 'auto' }}
                        exit={{ height: 0 }}
                        className="overflow-hidden"
                      >
                        <div className="p-3 md:p-4 border-t border-border bg-background overflow-y-auto max-h-[40vh]">
                          <div className="space-y-2 md:space-y-3">
                            {messages.map((msg: any, index: number) => {
                              const isExpanded = expandedMessageIndices.has(index);
                              const role = msg.role || 'unknown';
                              const content = msg.content;
                              const reasoning = msg.reasoning;
                              const toolCalls = msg.tool_calls;

                              return (
                                <div key={index} className="border border-border rounded-lg overflow-hidden">
                                  <button
                                    onClick={() => toggleMessage(index)}
                                    className="w-full flex items-center gap-2 md:gap-3 px-3 md:px-4 py-2 md:py-3 bg-muted/30 hover:bg-muted/50 transition-colors text-left"
                                  >
                                    {isExpanded ? <ChevronDown size={14} /> : <ChevronRight size={14} />}
                                    <span className={cn(
                                      "font-mono text-[10px] md:text-xs font-bold px-1.5 md:px-2 py-0.5 rounded",
                                      role === 'system' && "bg-red-500/20 text-red-400",
                                      role === 'user' && "bg-blue-500/20 text-blue-400",
                                      role === 'assistant' && "bg-green-500/20 text-green-400",
                                      role === 'tool' && "bg-purple-500/20 text-purple-400",
                                    )}>
                                      {role}
                                    </span>
                                    {reasoning && (
                                      <span className="text-[10px] bg-amber-500/20 text-amber-400 px-1.5 py-0.5 rounded">
                                        reasoning
                                      </span>
                                    )}
                                    <span className="text-xs md:text-sm text-muted-foreground truncate flex-1">
                                      {typeof content === 'string'
                                        ? content.substring(0, 100) + (content.length > 100 ? '...' : '')
                                        : Array.isArray(content) 
                                          ? `${content.length} parts`
                                          : 'empty'
                                      }
                                    </span>
                                    {toolCalls && (
                                      <span className="text-xs text-purple-400 ml-auto">
                                        {Array.isArray(toolCalls) ? toolCalls.length : 0} tool calls
                                      </span>
                                    )}
                                  </button>
                                  <AnimatePresence>
                                    {isExpanded && (
                                      <motion.div
                                        initial={{ height: 0 }}
                                        animate={{ height: 'auto' }}
                                        exit={{ height: 0 }}
                                        className="overflow-hidden"
                                      >
                                        <div className="p-3 md:p-4 border-t border-border bg-background overflow-y-auto max-h-[30vh]">
                                          {reasoning && (
                                            <div className="mb-4 pb-3 border-b border-border">
                                              <div className="text-[10px] font-bold text-amber-400 uppercase tracking-wider mb-2">Reasoning</div>
                                              <pre className="text-xs text-amber-300 whitespace-pre-wrap overflow-x-auto max-w-full">{reasoning}</pre>
                                            </div>
                                          )}
                                          <MessageContent content={content} />
                                          {toolCalls && Array.isArray(toolCalls) && toolCalls.length > 0 && (
                                            <div className="mt-4 pt-3 border-t border-border">
                                              <div className="text-[10px] font-bold text-purple-400 uppercase tracking-wider mb-2">Tool Calls</div>
                                              {toolCalls.map((tc: any, tcIdx: number) => (
                                                <div key={tcIdx} className="mb-3 p-3 bg-purple-500/10 rounded border border-purple-500/20 overflow-x-auto">
                                                  <div className="font-mono text-sm text-purple-300">{tc.function?.name || tc.name}</div>
                                                  <pre className="text-xs text-purple-400 mt-2 whitespace-pre-wrap break-all">
                                                    {JSON.stringify(tc.function?.arguments || tc.arguments || {}, null, 2)}
                                                  </pre>
                                                </div>
                                              ))}
                                            </div>
                                          )}
                                        </div>
                                      </motion.div>
                                    )}
                                  </AnimatePresence>
                                </div>
                              );
                            })}
                          </div>
                        </div>
                      </motion.div>
                    )}
                  </AnimatePresence>
                </div>
              )}
            </div>
          </div>

          {/* Footer */}
          <div className={cn("border-t border-border bg-muted/20", isMobile ? "px-4 py-3" : "px-6 py-4")}>
            <div className="flex items-center justify-between text-xs md:text-sm text-muted-foreground">
              <span>{messages?.length || 0} messages</span>
              {!isMobile && <span>Press ESC to close</span>}
            </div>
          </div>
        </motion.div>
      </motion.div>
    </AnimatePresence>
  );
}

export default ProviderRequestModal;
