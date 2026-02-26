"use client";

import React, { useRef, useEffect, useMemo, useCallback, useState } from "react";
import { motion, AnimatePresence } from "framer-motion";
import useAppStore, {
  selectFilteredMessages,
  selectAgentMetrics,
} from "../../stores/app-store";
import { useShallow } from "zustand/react/shallow";
import type { Message } from "../../types";
import { cn } from "../../lib/utils";

import {
  ChevronDown,
  ChevronRight,
  Cpu,
  Terminal,
  MessageSquare,
  ArrowDown,
  Undo2,
  Trash2,
  Edit3,
  Check,
  X,
  Bot,
  HammerIcon,
  Brain,
  Code,
} from "lucide-react";
import ProviderErrorBlock from "./ProviderErrorBlock";
import { MessageContextMenu, useLongPress } from "./MessageContextMenu";
import TurnBlock, { type Turn } from "./TurnBlock";
import { ProviderRequestModal } from "../modals/ProviderRequestModal";

const MemoizedTurnBlock = React.memo(TurnBlock);

export function MessageList(): React.ReactElement {
  const filteredMessages = useAppStore(useShallow(selectFilteredMessages));
  const metrics = useAppStore(useShallow(selectAgentMetrics));
  const agents = useAppStore((state) => state.agents);
  const activeThreadId = useAppStore((state) => state.activeThreadId);
  const deleteMessage = useAppStore((state) => state.deleteMessage);
  const restoreMessage = useAppStore((state) => state.restoreMessage);

  const parentRef = useRef<HTMLDivElement>(null);
  const [isUserScrolled, setIsUserScrolled] = useState(false);
  const [deletedMessageId, setDeletedMessageId] = useState<number | null>(null);
  const [providerRequestModal, setProviderRequestModal] = useState<{
    request: Record<string, unknown>;
  } | null>(null);

  const handleDelete = useCallback(
    (sequence: number) => {
      deleteMessage(sequence);
      setDeletedMessageId(sequence);
      setTimeout(() => {
        setDeletedMessageId((prev) => (prev === sequence ? null : prev));
      }, 5000);
    },
    [deleteMessage],
  );

  const lastTurnsRef = useRef<Turn[]>([]);
  const turns = useMemo(() => {
    const result: Turn[] = [];
    let currentTurn: Turn | null = null;
    let currentTurnCount = 0;

    filteredMessages.forEach((msg) => {
      const msgTurnCount = (msg as any).turnCount;

      if (msg.isUser) {
        if (currentTurn) {
          const activeAgentId = currentTurn.agentId;
          const isAgentWorking = activeAgentId
            ? agents.some((agent) => agent.id === activeAgentId && agent.status === "working")
            : false;
          if (!isAgentWorking) {
            currentTurn.isComplete = true;
          }
          result.push(currentTurn);
        }
        currentTurn = {
          id: `turn-${msg.sequence}`,
          userMessage: msg,
          workspace: [],
          isComplete: false,
        };
        currentTurnCount = msgTurnCount;
      } else {
        currentTurnCount = msgTurnCount;

        if (!currentTurn) {
          currentTurn = {
            id: `turn-agent-${msg.sequence}`,
            workspace: [],
            isComplete: false,
            agentId: msg.agentId,
            isAgentOnlyTurn: true,
          };
        } else if (currentTurn.userMessage && typeof currentTurn.userMessage.content === 'string' && currentTurn.userMessage.content.startsWith('Objective:')) {
        }

        const isFinalResponse = msg.type === "response" && !msg.isStreaming;

        if (isFinalResponse) {
          currentTurn.response = msg;
          currentTurn.isComplete = true;
          currentTurn.agentId = msg.agentId;

          const getTime = (t: Date | string | number) => t instanceof Date ? t.getTime() : new Date(t).getTime();
          const metric = metrics.find(
            (e) =>
              e.agentId === msg.agentId &&
              Math.abs(getTime(e.timestamp) - getTime(msg.timestamp)) < 5000,
          );
          if (metric) {
            currentTurn.executionTimeMs = metric.executionTimeMs;
          }

          result.push(currentTurn);
          currentTurn = null;
        } else {
          if (msg.type === "response" && msg.isStreaming) {
            currentTurn.response = msg;
          } else {
            currentTurn.workspace.push(msg);
          }
          currentTurn.agentId = msg.agentId;
        }
      }
    });

    if (currentTurn) {
      result.push(currentTurn);
    }

    const stabilized = result.map((nt) => {
      const pt = lastTurnsRef.current.find((t) => t.id === nt.id);
      if (!pt) return nt;

      const changed =
        nt.userMessage !== pt.userMessage ||
        nt.response !== pt.response ||
        nt.workspace.length !== pt.workspace.length ||
        nt.workspace.some((m, i) => m !== pt.workspace[i]) ||
        nt.isComplete !== pt.isComplete ||
        nt.executionTimeMs !== pt.executionTimeMs;

      return changed ? nt : pt;
    });

    lastTurnsRef.current = stabilized;
    return stabilized;
  }, [filteredMessages, metrics, agents]);

  const isStreaming = useMemo(() => {
    return filteredMessages.some((m) => m.isStreaming);
  }, [filteredMessages]);

  const scrollToBottom = useCallback((behavior: ScrollBehavior = "auto") => {
    if (parentRef.current) {
      parentRef.current.scrollTo({
        top: parentRef.current.scrollHeight,
        behavior,
      });
    }
  }, []);

  useEffect(() => {
    if (activeThreadId && filteredMessages.length > 0) {
      scrollToBottom("auto");
    }
  }, [activeThreadId]);

  const scrollSignal = useMemo(() => {
    const last = filteredMessages[filteredMessages.length - 1];
    if (!last) return "";
    const contentLen = typeof last.content === "string" ? last.content.length : 0;
    return `${last.sequence}-${last.isStreaming}-${contentLen}`;
  }, [filteredMessages]);

  useEffect(() => {
    if (!isUserScrolled) {
      scrollToBottom(isStreaming ? "smooth" : "auto");
    }
  }, [
    isUserScrolled,
    isStreaming,
    scrollToBottom,
    scrollSignal,
  ]);

  const handleScroll = (e: React.UIEvent<HTMLDivElement>) => {
    const target = e.target as HTMLDivElement;
    const { scrollTop, scrollHeight, clientHeight } = target;
    const isAtBottom = scrollHeight - scrollTop - clientHeight < 250;
    setIsUserScrolled(!isAtBottom);
  };

  if (turns.length === 0) {
    return (
      <div className="flex h-full items-center justify-center text-center">
        <div className="space-y-4">
          <div className="mx-auto flex h-12 w-12 items-center justify-center rounded-full bg-muted">
            <MessageSquare className="h-6 w-6 text-muted-foreground" />
          </div>
          <div>
            <p className="text-lg font-medium text-foreground">
              Codex Initialized
            </p>
            <p className="mt-1 text-sm text-muted-foreground">
              Awaiting objective or system command...
            </p>
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="relative mx-auto flex h-full flex-col max-w-4xl md:max-w-4xl sm:max-w-full">
      <div
        ref={parentRef}
        className="flex-1 overflow-y-auto overflow-x-hidden scrollbar-hide px-4"
        onScroll={handleScroll}
      >
        <div className="flex flex-col py-4 gap-4">
          {turns.map((turn, index) => (
            <MemoizedTurnBlock
              key={turn.id}
              turn={turn}
              isLastTurn={index === turns.length - 1}
              onDelete={handleDelete}
              onShowProviderRequest={setProviderRequestModal}
            />
          ))}
        </div>
      </div>

      <AnimatePresence>
        {deletedMessageId && (
          <motion.div
            initial={{ opacity: 0, y: 50, scale: 0.9 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{ opacity: 0, y: 20, scale: 0.9 }}
            className="absolute bottom-6 right-6 z-50 flex items-center gap-4 rounded-lg bg-foreground/90 px-4 py-3 text-sm font-medium text-background shadow-xl backdrop-blur-sm"
          >
            <span>Message deleted</span>
            <button
              onClick={() => {
                restoreMessage(deletedMessageId);
                setDeletedMessageId(null);
              }}
              className="rounded px-2 py-1 text-primary-foreground hover:bg-primary/20 hover:text-primary transition-colors font-bold"
            >
              Undo
            </button>
            <button
              onClick={() => setDeletedMessageId(null)}
              className="ml-2 text-muted-foreground hover:text-foreground"
            >
              <X size={14} />
            </button>
          </motion.div>
        )}

        {isUserScrolled && (
          <motion.button
            initial={{ opacity: 0, y: 10 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: 10 }}
            onClick={() => {
              setIsUserScrolled(false);
              scrollToBottom("smooth");
            }}
            className="absolute bottom-6 left-1/2 -translate-x-1/2 flex items-center gap-2 rounded-full bg-primary px-4 py-2 text-xs font-semibold text-primary-foreground shadow-2xl hover:scale-105 active:scale-95 transition-all z-50"
          >
            <ArrowDown className="h-3 w-3" />
            Scroll to bottom
          </motion.button>
        )}
      </AnimatePresence>

      {providerRequestModal && (
        <ProviderRequestModal
          isOpen={true}
          onClose={() => setProviderRequestModal(null)}
          request={providerRequestModal.request}
        />
      )}
    </div>
  );
}

export default MessageList;
