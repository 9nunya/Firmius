'use client';

import React, { useState, useEffect, useCallback } from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { motion, AnimatePresence } from 'framer-motion';
import type { Message } from '@firmius/shared/api';

interface MessageContextMenuProps {
  message: Message;
  isOpen: boolean;
  onClose: () => void;
  position: { x: number; y: number };
}

export function MessageContextMenu({
  message,
  isOpen,
  onClose,
  position,
}: MessageContextMenuProps) {
  const { activeThreadId, undoToMessage, undoLastTurn, branchThread } = useAppStore();
  const [isEditing, setIsEditing] = useState(false);
  const [editContent, setEditContent] = useState('');

  // Initialize edit content when menu opens
  useEffect(() => {
    if (isOpen && message.content) {
      const content = message.content;
      const contentStr = typeof content === 'string'
        ? content
        : Array.isArray(content)
          ? content.map(i => (i as any).text || '').join('\n')
          : '';
      setEditContent(contentStr);
    }
  }, [isOpen, message]);

  const handleUndo = async () => {
    await undoToMessage(message.sequence);
    onClose();
  };

  const handleUndoTurnAction = async () => {
    if (!message.agentId) return;
    await undoLastTurn(message.agentId);
    onClose();
  };

  const handleEdit = async () => {
    if (typeof editContent !== 'string' || !editContent.trim()) return;
    await branchThread(message.sequence, editContent);
    onClose();
    setIsEditing(false);
  };

  // Calculate menu position to keep it on screen
  const menuStyle = {
    left: Math.min(position.x, window.innerWidth - 200),
    top: Math.min(position.y, window.innerHeight - 150),
  };

  return (
    <AnimatePresence>
      {isOpen && (
        <>
          {/* Backdrop */}
          <motion.div
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            className="fixed inset-0 z-40"
            onClick={onClose}
          />

          {/* Menu */}
          <motion.div
            initial={{ opacity: 0, scale: 0.95 }}
            animate={{ opacity: 1, scale: 1 }}
            exit={{ opacity: 0, scale: 0.95 }}
            transition={{ duration: 0.15 }}
            className="fixed z-50 bg-background border border-border shadow-lg min-w-[180px]"
            style={menuStyle}
          >
            {isEditing ? (
              <div className="p-3 space-y-2">
                <textarea
                  value={typeof editContent === 'string' ? editContent : ''}
                  onChange={(e) => setEditContent(e.target.value)}
                  className="w-full h-24 p-2 text-sm bg-muted resize-none outline-none"
                  autoFocus
                />
                <div className="flex gap-2">
                  <button
                    onClick={() => {
                      setIsEditing(false);
                      onClose();
                    }}
                    className="flex-1 px-3 py-1.5 text-xs bg-muted hover:bg-muted/80"
                  >
                    Cancel
                  </button>
                  <button
                    onClick={handleEdit}
                    className="flex-1 px-3 py-1.5 text-xs bg-primary text-primary-foreground hover:bg-primary/90"
                  >
                    Send
                  </button>
                </div>
              </div>
            ) : (
              <div className="py-1">
                {message.isUser && (
                  <>
                    <button
                      onClick={() => setIsEditing(true)}
                      className="w-full px-4 py-2 text-left text-sm hover:bg-accent/50 transition-colors flex items-center gap-2"
                    >
                      <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M11 5H6a2 2 0 00-2 2v11a2 2 0 002 2h11a2 2 0 002-2v-5m-1.414-9.414a2 2 0 112.828 2.828L11.828 15H9v-2.828l8.586-8.586z" />
                      </svg>
                      Edit & Resend
                    </button>
                    <button
                      onClick={handleUndo}
                      className="w-full px-4 py-2 text-left text-sm hover:bg-accent/50 transition-colors flex items-center gap-2"
                    >
                      <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M3 10h10a8 8 0 018 8v2M3 10l6 6m-6-6l6-6" />
                      </svg>
                      Undo to Here
                    </button>
                  </>
                )}

                {!message.isUser && message.type === 'response' && (
                  <button
                    onClick={handleUndoTurnAction}
                    className="w-full px-4 py-2 text-left text-sm hover:bg-accent/50 transition-colors flex items-center gap-2"
                  >
                    <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12.066 11.2a1 1 0 000 1.6l5.334 4A1 1 0 0019 16V8a1 1 0 00-1.6-.8l-5.333 4zM4.066 11.2a1 1 0 000 1.6l5.334 4A1 1 0 0011 16V8a1 1 0 00-1.6-.8l-5.334 4z" />
                    </svg>
                    Undo Last Turn
                  </button>
                )}
              </div>
            )}
          </motion.div>
        </>
      )}
    </AnimatePresence>
  );
}

// Hook to handle long press detection
export function useLongPress(callback: () => void, ms = 500) {
  const [startLongPress, setStartLongPress] = useState(false);

  useEffect(() => {
    let timerId: NodeJS.Timeout;

    if (startLongPress) {
      timerId = setTimeout(callback, ms);
    }

    return () => {
      clearTimeout(timerId);
    };
  }, [startLongPress, callback, ms]);

  return {
    onMouseDown: () => setStartLongPress(true),
    onMouseUp: () => setStartLongPress(false),
    onMouseLeave: () => setStartLongPress(false),
    onTouchStart: () => setStartLongPress(true),
    onTouchEnd: () => setStartLongPress(false),
  };
}

export default MessageContextMenu;
