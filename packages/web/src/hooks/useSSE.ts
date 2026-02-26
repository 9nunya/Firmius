'use client';

import { useEffect, useCallback, useRef } from 'react';
import useAppStore from '../stores/app-store';
import { sseClient, type SSEStatus } from '@firmius/shared/sse';
import type { Event } from '../types';

export function useSSE() {
  const { activeThreadId, updateFromEvent } = useAppStore();
  const connectionStatusRef = useRef<SSEStatus>('disconnected');
  const eventHandlersRef = useRef<Map<string, (event: Event) => void>>(new Map());
  const statusHandlersRef = useRef<Array<(status: SSEStatus) => void>>([]);
  const errorHandlersRef = useRef<Array<(error: Error) => void>>([]);

  const connect = useCallback((threadId: string) => {
    sseClient.connect(threadId);
  }, []);

  const disconnect = useCallback(() => {
    sseClient.disconnect();
  }, []);

  const onEvent = useCallback((eventType: string, handler: (event: Event) => void) => {
    eventHandlersRef.current.set(eventType, handler);
    return () => {
      eventHandlersRef.current.delete(eventType);
    };
  }, []);

  const onStatusChange = useCallback((handler: (status: SSEStatus) => void) => {
    statusHandlersRef.current.push(handler);
    return () => {
      statusHandlersRef.current = statusHandlersRef.current.filter((h) => h !== handler);
    };
  }, []);

  const onError = useCallback((handler: (error: Error) => void) => {
    errorHandlersRef.current.push(handler);
    return () => {
      errorHandlersRef.current = errorHandlersRef.current.filter((h) => h !== handler);
    };
  }, []);

  const getConnectionStatus = useCallback((): SSEStatus => {
    return connectionStatusRef.current;
  }, []);

  useEffect(() => {
    const unsubscribe = sseClient.onParsedMessage((event: any) => {
      updateFromEvent(event);

      const handler = eventHandlersRef.current.get(event.type);
      if (handler) {
        handler(event);
      }
    });

    return unsubscribe;
  }, [updateFromEvent]);

  useEffect(() => {
    const unsubscribe = sseClient.onStatusChange((status: SSEStatus) => {
      connectionStatusRef.current = status;
      statusHandlersRef.current.forEach((handler) => {
        handler(status);
      });
    });

    return unsubscribe;
  }, []);

  useEffect(() => {
    const unsubscribe = sseClient.onError((error: Error) => {
      errorHandlersRef.current.forEach((handler) => {
        handler(error);
      });
    });

    return unsubscribe;
  }, []);

  useEffect(() => {
    if (activeThreadId) {
      connect(activeThreadId);
    } else {
      disconnect();
    }

    return () => {
      disconnect();
    };
  }, [activeThreadId, connect, disconnect]);

  return {
    connectionStatus: connectionStatusRef.current,
    connect,
    disconnect,
    onEvent,
    onStatusChange,
    onError,
    getConnectionStatus
  };
}
