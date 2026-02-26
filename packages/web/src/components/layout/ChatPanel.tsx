'use client';

import React from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { HomeScreen } from './HomeScreen';
import { LoadingScreen } from '@/components/ui/LoadingScreen';
import { AnimatePresence } from 'framer-motion';

interface ChatPanelProps extends React.PropsWithChildren {
  className?: string;
  onCreateThread?: () => void;
}

export function ChatPanel({ className, children, onCreateThread }: ChatPanelProps): React.ReactElement {
  const { activeThreadId, messages } = useAppStore();
  const [isLoadingThread, setIsLoadingThread] = React.useState(false);
  const prevThreadIdRef = React.useRef<string | null>(null);

  // Handle thread switching loading state
  React.useEffect(() => {
    if (activeThreadId && activeThreadId !== prevThreadIdRef.current) {
      setIsLoadingThread(true);
      prevThreadIdRef.current = activeThreadId;

      // Fallback timeout in case messages are empty for a valid reason
      const timer = setTimeout(() => {
        setIsLoadingThread(false);
      }, 1500);
      return () => clearTimeout(timer);
    }
  }, [activeThreadId]);

  // Turn off loading when messages arrive or if it's explicitly an empty loaded thread
  React.useEffect(() => {
    if (messages.length > 0) {
      setIsLoadingThread(false);
    }
  }, [messages.length]);

  // Show HomeScreen when no thread is selected
  if (!activeThreadId) {
    return (
      <div className={cn('flex h-full overflow-hidden bg-background', className)}>
        <HomeScreen onCreateThread={onCreateThread} />
      </div>
    );
  }

  return (
    <div
      className={cn(
        'flex h-full overflow-hidden bg-background relative',
        className
      )}
    >
      <AnimatePresence>
        {isLoadingThread && (
          <LoadingScreen message="Loading thread..." />
        )}
      </AnimatePresence>

      <div className="flex-1 overflow-hidden">
        {messages.length === 0 ? (
          <div className="flex h-full items-center justify-center text-center p-4">
            <div>
              <p className="text-lg font-medium text-foreground">No messages yet</p>
              <p className="mt-2 text-sm text-muted-foreground">
                Send a message to start the conversation
              </p>
            </div>
          </div>
        ) : (
          <div className="h-full p-4 pb-[180px] md:pb-4 overflow-y-auto">
            {children}
          </div>
        )}
      </div>
    </div>
  );
}

export default ChatPanel;
