'use client';

import { useMemo, useCallback } from 'react';
import type { Message } from '@firmius/shared/api';

export interface TurnInfo {
  id: string;
  startSequence: number;
  endSequence: number | null;
  userMessage: Message | null;
  workspaceMessages: Message[];
  responseMessage: Message | null;
  isComplete: boolean;
}

export function useTurnTracking(messages: Message[]) {
  const turns = useMemo(() => {
    const turns: TurnInfo[] = [];
    let currentTurn: TurnInfo | null = null;

    for (const message of messages) {
      // User message starts a new turn
      if (message.isUser) {
        // Complete previous turn if exists
        if (currentTurn) {
          currentTurn.endSequence = message.sequence - 1;
          turns.push(currentTurn);
        }

        // Start new turn
        currentTurn = {
          id: `turn-${message.sequence}`,
          startSequence: message.sequence,
          endSequence: null,
          userMessage: message,
          workspaceMessages: [],
          responseMessage: null,
          isComplete: false,
        };
      } else if (currentTurn) {
        // Agent message belongs to current turn
        if (message.type === 'monologue' || (message.thinking && !message.content)) {
          // Workspace/thinking message
          currentTurn.workspaceMessages.push(message);
        } else {
          // Response message completes the turn
          currentTurn.responseMessage = message;
          currentTurn.endSequence = message.sequence;
          currentTurn.isComplete = true;
          turns.push(currentTurn);
          currentTurn = null;
        }
      }
    }

    // Handle incomplete turn at end
    if (currentTurn) {
      turns.push(currentTurn);
    }

    return turns;
  }, [messages]);

  const getTurnForMessage = useCallback((sequence: number): TurnInfo | null => {
    return turns.find(
      turn => turn.startSequence <= sequence && 
      (turn.endSequence === null || turn.endSequence >= sequence)
    ) || null;
  }, [turns]);

  const getCurrentTurn = useCallback((): TurnInfo | null => {
    return turns[turns.length - 1] || null;
  }, [turns]);

  const isTurnComplete = useCallback((turnId: string): boolean => {
    const turn = turns.find(t => t.id === turnId);
    return turn?.isComplete ?? false;
  }, [turns]);

  const getTurnsForAgent = useCallback((agentId: string): TurnInfo[] => {
    return turns.filter(turn => 
      turn.workspaceMessages.some(m => m.agentId === agentId) ||
      turn.responseMessage?.agentId === agentId
    );
  }, [turns]);

  return {
    turns,
    getTurnForMessage,
    getCurrentTurn,
    isTurnComplete,
    getTurnsForAgent,
  };
}
