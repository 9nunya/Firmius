"use client";

import React, { useRef, useEffect, useMemo, useCallback, useState } from "react";
import { motion, AnimatePresence } from "framer-motion";
import useAppStore from "../../stores/app-store";
import type { Message } from "../../types";
import { cn } from "../../lib/utils";

import {
  ChevronDown,
  ChevronRight,
  Bot,
  HammerIcon,
  Brain,
  MessageSquare,
  Undo2,
  Trash2,
  Edit3,
  Check,
  X,
} from "lucide-react";
import { MarkdownContent } from "./MarkdownContent";
import { MessageContextMenu } from "./MessageContextMenu";
import ToolCallStream from "./ToolCallStream";

export interface Turn {
  id: string;
  userMessage?: Message;
  workspace: Message[];
  response?: Message;
  isComplete: boolean;
  agentId?: string;
  executionTimeMs?: number;
  isAgentOnlyTurn?: boolean;
}

function MessageContent({
  content,
  className,
}: {
  content: string | unknown[];
  className?: string;
}) {
  if (typeof content === "string") {
    return <MarkdownContent content={content} className={className} />;
  }

  if (!Array.isArray(content)) {
    return (
      <MarkdownContent
        content={JSON.stringify(content)}
        className={className}
      />
    );
  }

  return (
    <div className="flex flex-col gap-3">
      {content.map((item, i) => {
        const typedItem = item as {
          type: string;
          text?: string;
          image_url?: { url: string };
        };
        if (typedItem.type === "text" && typedItem.text) {
          return (
            <MarkdownContent
              key={i}
              content={typedItem.text}
              className={className}
            />
          );
        }
        if (typedItem.type === "image_url" && typedItem.image_url) {
          return (
            /* eslint-disable-next-line @next/next/no-img-element */
            <img
              key={i}
              src={typedItem.image_url.url}
              alt="Uploaded content"
              className="max-w-full rounded-lg border border-border/50 shadow-sm"
              loading="lazy"
            />
          );
        }
        return null;
      })}
    </div>
  );
}

function isViewingSubagent(
  turn: Turn,
  agents: any[],
  activeAgentId: string | null,
): any | null {
  if (!activeAgentId) return null;
  const agent = agents.find((a) => a.id === activeAgentId);
  return agent && !agent.isLead ? agent : null;
}

export function TurnBlock({
  turn,
  isLastTurn,
  onDelete,
  onShowProviderRequest,
}: {
  turn: Turn;
  isLastTurn: boolean;
  onDelete: (seq: number) => void;
  onShowProviderRequest: (data: { request: Record<string, unknown> }) => void;
}) {
  const branchThread = useAppStore((state) => state.branchThread);
  const undoToMessage = useAppStore((state) => state.undoToMessage);
  const agents = useAppStore((state) => state.agents);
  const activeAgentId = useAppStore((state) => state.activeAgentId);
  const activeThreadId = useAppStore((state) => state.activeThreadId);
  const editMessage = useAppStore((state) => state.editMessage);
  const [isWorkspaceExpanded, setIsWorkspaceExpanded] = useState(
    !turn.isComplete,
  );
  const [isEditing, setIsEditing] = useState(false);
  const [editContent, setEditContent] = useState("");
  const [contextMenu, setContextMenu] = useState<{
    isOpen: boolean;
    message: Message | null;
    position: { x: number; y: number };
  }>({ isOpen: false, message: null, position: { x: 0, y: 0 } });

  const hasWorkspaceContent = turn.workspace.some((msg) =>
    msg.thinking ||
    msg.providerRequest ||
    msg.providerError ||
    (msg.content && typeof msg.content === "string" && msg.content.length > 0) ||
    (msg.toolCalls && msg.toolCalls.length > 0)
  );

  const handleContextMenu = useCallback((e: React.MouseEvent, message: Message) => {
    e.preventDefault();
    setContextMenu({
      isOpen: true,
      message,
      position: { x: e.clientX, y: e.clientY }
    });
  }, []);

  useEffect(() => {
    if (!contextMenu.isOpen) return;
    const handleClick = () => setContextMenu(prev => ({ ...prev, isOpen: false }));
    const handleEscape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setContextMenu(prev => ({ ...prev, isOpen: false }));
    };
    window.addEventListener('click', handleClick);
    window.addEventListener('keydown', handleEscape);
    return () => {
      window.removeEventListener('click', handleClick);
      window.removeEventListener('keydown', handleEscape);
    };
  }, [contextMenu.isOpen]);

  useEffect(() => {
    if (turn.isComplete) {
      const timer = setTimeout(() => {
        setIsWorkspaceExpanded(false);
      }, 500);
      return () => clearTimeout(timer);
    }
  }, [turn.isComplete]);

  useEffect(() => {
    if (isEditing && turn.userMessage) {
      const content = turn.userMessage.content;
      const contentStr =
        typeof content === "string"
          ? content
          : (content as Array<{ type: string; text?: string }>)
            .map((item) => (item.type === "text" ? item.text : ""))
            .join("\n");
      setEditContent(contentStr);
    }
  }, [isEditing, turn.userMessage]);

  const handleSaveEdit = () => {
    if (!turn.userMessage) return;

    if (isLastTurn) {
      editMessage(turn.userMessage.sequence, editContent);
    } else {
      branchThread(turn.userMessage.sequence, editContent);
    }
    setIsEditing(false);
  };

  const executionTime = turn.executionTimeMs
    ? `${(turn.executionTimeMs / 1000).toFixed(1)}s`
    : "";

  const viewingSubagent = isViewingSubagent(turn, agents, activeAgentId);

  const handleWorkspaceToggle = useCallback(() => {
    setIsWorkspaceExpanded(prev => !prev);
  }, []);

  return (
    <motion.div
      initial={{ opacity: 0, y: 20 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.4, ease: "easeOut" }}
      className="flex flex-col gap-6"
    >
      {isLastTurn && viewingSubagent && (
        <div className="rounded-xl border border-primary/30 bg-primary/5 p-4">
          <div className="flex items-center gap-2 mb-2">
            <Bot className="h-4 w-4 text-primary" />
            <span className="text-xs font-bold uppercase tracking-widest text-primary">
              {viewingSubagent.readableName}
            </span>
          </div>
          {viewingSubagent.objective && (
            <div className="text-sm text-foreground/80 mb-2">
              <span className="font-semibold">Objective: </span>
              {viewingSubagent.objective}
            </div>
          )}
          {viewingSubagent.purpose && (
            <div className="text-xs text-muted-foreground">
              <span className="font-semibold">Purpose: </span>
              {viewingSubagent.purpose}
            </div>
          )}
          {viewingSubagent.parentId && (
            <div className="mt-2 pt-2 border-t border-primary/10 text-xs text-muted-foreground">
              Parent:{" "}
              {agents.find((a) => a.id === viewingSubagent.parentId)
                ?.readableName || viewingSubagent.parentId}
            </div>
          )}
        </div>
      )}

      {turn.userMessage && (
        <div
          className="flex justify-end pr-2 group relative"
          onContextMenu={(e) => handleContextMenu(e, turn.userMessage!)}
        >
          <div className="absolute right-full top-1/2 -translate-y-1/2 mr-2 flex items-center gap-0.5 opacity-100 md:opacity-0 md:group-hover:opacity-100 transition-all duration-200">
            <button
              onClick={(e) => {
                e.preventDefault();
                setIsEditing(true);
              }}
              className="p-2 hover:bg-muted text-muted-foreground hover:text-indigo-400 transition-colors"
              title="Edit message"
            >
              <Edit3 size={14} />
            </button>
            <button
              onClick={(e) => {
                e.preventDefault();
                const content = turn.userMessage!.content;
                const contentStr =
                  typeof content === "string"
                    ? content
                    : (content as Array<{ type: string; text?: string }>)
                      .map((item) =>
                        item.type === "text" ? item.text : "",
                      )
                      .join("\n");
                branchThread(turn.userMessage!.sequence, contentStr);
              }}
              className="p-2 hover:bg-muted text-muted-foreground hover:text-amber-400 transition-colors"
              title="Retry (undo agent response and resend)"
            >
              <Undo2 size={14} />
            </button>
            <button
              onClick={(e) => {
                e.preventDefault();
                if (confirm('Undo this turn? This will remove your message and the agent\'s response from the thread.')) {
                  undoToMessage(turn.userMessage!.sequence);
                }
              }}
              className="p-2 hover:bg-destructive/10 text-muted-foreground hover:text-destructive transition-colors"
              title="Undo turn (remove message + response)"
            >
              <Trash2 size={14} />
            </button>
          </div>

          <motion.div
            initial={{ scale: 0.95, opacity: 0 }}
            animate={{ scale: 1, opacity: 1 }}
            transition={{ delay: 0.1 }}
            className={cn(
              "max-w-[95%] md:max-w-[85%] font-medium transition-all",
              isEditing
                ? "w-full bg-background border border-border p-2"
                : "bg-foreground text-background px-4 py-3 md:px-5",
            )}
          >
            {isEditing ? (
              <div className="flex flex-col gap-2">
                <textarea
                  value={editContent}
                  onChange={(e) => setEditContent(e.target.value)}
                  className="w-full min-h-[100px] resize-y bg-transparent p-2 text-sm outline-none placeholder:text-muted-foreground md:text-base"
                  autoFocus
                  onKeyDown={(e) => {
                    if (e.key === "Enter" && e.metaKey) {
                      handleSaveEdit();
                    } else if (e.key === "Escape") {
                      setIsEditing(false);
                    }
                  }}
                />
                <div className="flex justify-end gap-2">
                  <button
                    onClick={() => setIsEditing(false)}
                    className="flex items-center gap-1 rounded-md px-3 py-1.5 text-xs font-medium text-muted-foreground hover:bg-muted"
                  >
                    <X size={14} />
                    Cancel
                  </button>
                  <button
                    onClick={handleSaveEdit}
                    className="flex items-center gap-1 rounded-md bg-primary px-3 py-1.5 text-xs font-medium text-primary-foreground hover:bg-primary/90"
                  >
                    <Check size={14} />
                    Save
                  </button>
                </div>
              </div>
            ) : (
              <MessageContent
                content={turn.userMessage.content}
                className="text-sm md:text-base"
              />
            )}
          </motion.div>
        </div>
      )}

      {(hasWorkspaceContent || turn.isAgentOnlyTurn) && (
        <div className="flex flex-col gap-3">
          <div className="flex items-center gap-3 opacity-20 hover:opacity-100 transition-opacity px-4">
            <div className="h-[1px] flex-1 bg-border/60" />
            <button
              className="flex items-center gap-1.5 cursor-pointer hover:text-muted-foreground transition-colors outline-none py-1"
              onClick={handleWorkspaceToggle}
            >
              <HammerIcon className="h-3 w-3 text-gray-500" />
              <span className="text-[10px] font-bold uppercase tracking-[0.2em]">
                {turn.isAgentOnlyTurn ? "AGENT ACTIVITY" : "WORK HISTORY"}
                {executionTime ? (
                  `(${executionTime})`
                ) : turn.isComplete ? (
                  ""
                ) : (
                  <span className="animate-pulse">...</span>
                )}
              </span>

              {isWorkspaceExpanded ? (
                <ChevronDown className="h-3 w-3" />
              ) : (
                <ChevronRight className="h-3 w-3" />
              )}
            </button>
            <div className="h-[1px] flex-1 bg-border/60" />
          </div>

          <div className="relative ml-2 pl-4 border-l border-indigo-500/10 flex flex-col">
            <AnimatePresence initial={false}>
              {isWorkspaceExpanded && (
                <motion.div
                  key="workspace-content"
                  initial={{ height: 0, opacity: 0 }}
                  animate={{ height: "auto", opacity: 1 }}
                  exit={{ height: 0, opacity: 0 }}
                  transition={{ duration: 0.25, ease: "easeInOut" }}
                  className="overflow-hidden"
                >
                  <div className="flex flex-col gap-4 py-3">
                    {turn.workspace.map((msg, index) => {
                      const prevMsg = index > 0 ? turn.workspace[index - 1] : null;
                      const currentTurn = (msg as any).turnCount;
                      const prevTurn = prevMsg ? (prevMsg as any).turnCount : null;
                      const showTurnSeparator = prevTurn !== null && currentTurn !== prevTurn;

                      return (
                        <React.Fragment key={msg.sequence}>
                          {showTurnSeparator && (
                            <motion.div
                              initial={{ opacity: 0, scale: 0.9, y: -10 }}
                              animate={{ opacity: 1, scale: 1, y: 0 }}
                              transition={{ duration: 0.3, ease: "easeOut" }}
                              className="flex items-center gap-3 py-2"
                            >
                              <div className="h-px flex-1 bg-gradient-to-r from-transparent via-border to-border" />
                              <span className="text-[10px] font-mono font-medium text-muted-foreground/70 uppercase tracking-wider">
                                Turn {currentTurn}
                              </span>
                              <div className="h-px flex-1 bg-gradient-to-l from-transparent via-border to-border" />
                            </motion.div>
                          )}
                          {msg.thinking && <ThinkingBlock thought={msg.thinking} noBorder />}
                          {msg.type === "provider_request" &&
                            msg.providerRequest && (
                              <ProviderRequestBlock
                                request={msg.providerRequest}
                                onClick={() =>
                                  onShowProviderRequest({
                                    request: msg.providerRequest!,
                                  })
                                }
                              />
                            )}
                          {msg.isMonologue && (
                            <MonologueBlock
                              content={msg.content}
                              isStreaming={msg.isStreaming}
                              noBorder
                            />
                          )}
                          {msg.type === "provider_error" && msg.providerError && (
                            <ProviderErrorBlock error={msg.providerError} />
                          )}
                          {msg.toolCalls && msg.toolCalls.length > 0 && (
                            <ToolCallStream toolCalls={msg.toolCalls} />
                          )}
                        </React.Fragment>
                      );
                    })}

                    {turn.response?.thinking && (
                      <ThinkingBlock thought={turn.response.thinking} noBorder />
                    )}
                  </div>
                </motion.div>
              )}
            </AnimatePresence>
          </div>
        </div>
      )}

      {turn.response && (
        <div
          className="flex justify-start pl-2"
          onContextMenu={(e) => handleContextMenu(e, turn.response!)}
        >
          <motion.div
            initial={{ opacity: 0, x: -10 }}
            animate={{ opacity: 1, x: 0 }}
            transition={{ delay: 0.2 }}
            className="max-w-[95%] md:max-w-[85%] bg-muted/30 border border-border px-4 py-3 md:px-6 md:py-4 text-foreground"
          >
            <div className="relative">
              {turn.response.toolCalls && turn.response.toolCalls.length > 0 && (
                <div className="mb-3">
                  <ToolCallStream toolCalls={turn.response.toolCalls} />
                </div>
              )}
              <MessageContent
                content={turn.response.content}
                className="text-sm md:text-base leading-relaxed"
              />
              {turn.response.isStreaming && (
                <motion.span
                  animate={{ opacity: [0, 1, 0] }}
                  transition={{ duration: 0.8, repeat: Infinity }}
                  className="inline-block ml-1 h-4 w-1.5 bg-indigo-500 align-middle absolute bottom-1"
                />
              )}
            </div>
          </motion.div>
        </div>
      )}

      {contextMenu.isOpen && contextMenu.message && (
        <MessageContextMenu
          message={contextMenu.message}
          isOpen={contextMenu.isOpen}
          onClose={() => setContextMenu(prev => ({ ...prev, isOpen: false }))}
          position={contextMenu.position}
        />
      )}
    </motion.div>
  );
}

function ThinkingBlock({ thought, noBorder }: { thought: string; noBorder?: boolean }) {
  return (
    <div className={cn(
      "pl-4 py-1 text-sm italic text-muted-foreground/70",
      !noBorder && "border-l-2 border-gray-500/30"
    )}>
      <div className="flex items-center gap-2 mb-1 not-italic font-semibold text-gray-600/50 text-[10px] uppercase tracking-widest">
        <Brain className="h-3 w-3" />
        Thinking
      </div>
      <MarkdownContent content={thought} />
    </div>
  );
}

function MonologueBlock({
  content,
  isStreaming,
  noBorder
}: {
  content: string | unknown[];
  isStreaming?: boolean;
  noBorder?: boolean;
}) {
  if (
    !content ||
    (typeof content === "string" && !content.trim() && !isStreaming)
  )
    return null;

  return (
    <div className={cn(
      "pl-4 py-1 text-sm italic text-muted-foreground/60 leading-relaxed relative group",
      !noBorder && "border-l-2 border-border/50"
    )}>
      <div className="flex items-center gap-2 mb-1 opacity-40 group-hover:opacity-100 transition-opacity">
        <MessageSquare className="h-3 w-3" />
        <span className="text-[10px] font-bold uppercase tracking-widest">
          Monologue
        </span>
      </div>
      <MessageContent content={content} />
      {isStreaming && (
        <motion.span
          animate={{ opacity: [0, 1, 0] }}
          transition={{ duration: 0.8, repeat: Infinity }}
          className="inline-block ml-1 h-3 w-1 bg-muted-foreground/40 align-middle"
        />
      )}
    </div>
  );
}

function ProviderRequestBlock({
  request,
  onClick,
}: {
  request: Record<string, unknown>;
  onClick: () => void;
}) {
  const messages = request.messages as unknown[] | undefined;
  const model = request.model as string | undefined;
  const temperature = request.temperature as number | undefined;

  return (
    <button
      onClick={onClick}
      className="flex items-center gap-2 p-3 border border-border hover:bg-accent/50 transition-colors text-left w-full"
    >
      <div className="p-1.5 bg-muted">
        <CodeIcon size={14} className="text-muted-foreground" />
      </div>
      <div className="flex-1 min-w-0">
        <div className="text-[10px] font-bold text-muted-foreground/60 uppercase tracking-wider">
          Provider Request
        </div>
        <div className="text-xs text-muted-foreground truncate">
          {model || "unknown model"} • {messages?.length || 0} messages
          {temperature != null ? ` • temp: ${temperature}` : ""}
        </div>
      </div>
      <span className="text-[10px] text-muted-foreground/40">View details</span>
    </button>
  );
}

function CodeIcon({ size, className }: { size: number; className?: string }) {
  return (
    <svg
      xmlns="http://www.w3.org/2000/svg"
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      className={className}
    >
      <polyline points="16 18 22 12 16 6" />
      <polyline points="8 6 2 12 8 18" />
    </svg>
  );
}

function ProviderErrorBlock({ error }: { error: { error: string; modelId?: string; providerId?: string } }) {
  return (
    <div className="p-3 border border-red-500/30 bg-red-500/10 rounded-md">
      <div className="text-xs font-bold text-red-400 uppercase tracking-wider">
        Provider Error
      </div>
      <div className="text-sm text-red-300 mt-1">{error.error}</div>
      {error.modelId && (
        <div className="text-xs text-muted-foreground mt-1">
          Model: {error.modelId}
        </div>
      )}
    </div>
  );
}

export default React.memo(TurnBlock, (prev, next) => {
  if (prev.turn.id !== next.turn.id) return false;
  if (prev.turn.isComplete !== next.turn.isComplete) return false;
  if (prev.turn.workspace.length !== next.turn.workspace.length) return false;
  if (prev.turn.response !== next.turn.response) return false;
  if (prev.isLastTurn !== next.isLastTurn) return false;
  for (let i = 0; i < prev.turn.workspace.length; i++) {
    if (prev.turn.workspace[i] !== next.turn.workspace[i]) return false;
  }
  return true;
});
