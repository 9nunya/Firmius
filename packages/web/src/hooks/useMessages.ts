'use client';

import { useEffect, useCallback } from 'react';
import useAppStore from '../stores/app-store';
import { client } from '@firmius/shared/api';
import type { Message } from '@firmius/shared/api';

export function useMessages() {
  const { activeThreadId, messages, sendMessage, editMessage, deleteMessage } = useAppStore();

  const loadMessages = useCallback(async (threadId: string) => {
    try {
      const loadedMessages = await client.getMessages(threadId);
      return loadedMessages;
    } catch (error) {
      console.error('Failed to load messages:', error);
      return [];
    }
  }, []);

  const sendMessageWithOptimisticUpdate = useCallback(async (content: string) => {
    if (!activeThreadId) return;

    const tempMessage: Message = {
      sequence: messages.length + 1,
      isUser: true,
      content,
      timestamp: new Date(),
      tokens: 0,
      type: 'response',
    };

    await sendMessage(content);
  }, [activeThreadId, messages.length, sendMessage]);

  const editMessageWithOptimisticUpdate = useCallback(async (sequence: number, content: string) => {
    await editMessage(sequence, content);
  }, [editMessage]);

  const deleteMessageWithOptimisticUpdate = useCallback(async (sequence: number) => {
    await deleteMessage(sequence);
  }, [deleteMessage]);

  const getFilteredMessages = useCallback((agentId: string | null) => {
    if (!agentId) return messages;
    return messages.filter((msg) => !msg.isUser);
  }, [messages]);

  return {
    messages,
    loadMessages,
    sendMessage: sendMessageWithOptimisticUpdate,
    editMessage: editMessageWithOptimisticUpdate,
    deleteMessage: deleteMessageWithOptimisticUpdate,
    getFilteredMessages
  };
}
