'use client';

import React from 'react';
import { motion, AnimatePresence } from 'framer-motion';
import { cn } from '@/lib/utils';
import { StreamingText } from './StreamingText';

export interface LiveActivity {
  type: 'thinking' | 'tool' | 'response';
  content: string;
  timestamp: number;
  toolName?: string;
  filePath?: string;
}

export interface LiveActivityViewProps {
  activities: LiveActivity[];
  maxActivities?: number;
  className?: string;
}

const activityIcons = {
  thinking: (
    <svg className="w-3.5 h-3.5 text-amber-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M9.663 17h4.673M12 3v1m6.364 1.636l-.707.707M21 12h-1M4 12H3m3.343-5.657l-.707-.707m2.828 9.9a5 5 0 117.072 0l-.548.547A3.374 3.374 0 0014 18.469V19a2 2 0 11-4 0v-.531c0-.895-.356-1.754-.988-2.386l-.548-.547z" />
    </svg>
  ),
  tool: (
    <svg className="w-3.5 h-3.5 text-blue-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M10.325 4.317c.426-1.756 2.924-1.756 3.35 0a1.724 1.724 0 002.573 1.066c1.543-.94 3.31.826 2.37 2.37a1.724 1.724 0 001.065 2.572c1.756.426 1.756 2.924 0 3.35a1.724 1.724 0 00-1.066 2.573c.94 1.543-.826 3.31-2.37 2.37a1.724 1.724 0 00-2.572 1.065c-.426 1.756-2.924 1.756-3.35 0a1.724 1.724 0 00-2.573-1.066c-1.543.94-3.31-.826-2.37-2.37a1.724 1.724 0 00-1.065-2.572c-1.756-.426-1.756-2.924 0-3.35a1.724 1.724 0 001.066-2.573c-.94-1.543.826-3.31 2.37-2.37.996.608 2.296.07 2.572-1.065z" />
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
    </svg>
  ),
  response: (
    <svg className="w-3.5 h-3.5 text-green-500" fill="none" viewBox="0 0 24 24" stroke="currentColor">
      <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M8 10h.01M12 10h.01M16 10h.01M9 16H5a2 2 0 01-2-2V6a2 2 0 012-2h14a2 2 0 012 2v8a2 2 0 01-2 2h-5l-5 5v-5z" />
    </svg>
  ),
};

export function LiveActivityView({
  activities,
  maxActivities = 3,
  className,
}: LiveActivityViewProps) {
  // Show only the most recent activities, up to maxActivities
  const recentActivities = activities.slice(-maxActivities);

  return (
    <div className={cn('space-y-2', className)}>
      <AnimatePresence mode="popLayout">
        {recentActivities.map((activity, index) => (
          <motion.div
            key={`${activity.type}-${activity.timestamp}-${index}`}
            layout
            initial={{ opacity: 0, y: 10, height: 0 }}
            animate={{ opacity: 1, y: 0, height: 'auto' }}
            exit={{ opacity: 0, height: 0 }}
            transition={{ duration: 0.15, ease: 'easeOut' }}
            className="flex items-start gap-2 text-sm"
          >
            {/* Icon */}
            <span className="flex-shrink-0 mt-0.5">{activityIcons[activity.type]}</span>

            {/* Content */}
            <div className="flex-1 min-w-0">
              {activity.type === 'thinking' && (
                <div className="text-muted-foreground">
                  <StreamingText
                    content={activity.content}
                    isStreaming={true}
                    speed="fast"
                    className="text-sm"
                  />
                </div>
              )}

              {activity.type === 'tool' && (
                <div className="flex items-center gap-2 flex-wrap">
                  <span className="text-muted-foreground">{activity.toolName}</span>
                  {activity.filePath && (
                    <span className="text-foreground font-mono text-xs truncate">
                      {activity.filePath}
                    </span>
                  )}
                </div>
              )}

              {activity.type === 'response' && (
                <div className="text-foreground/80">
                  <StreamingText
                    content={activity.content}
                    isStreaming={true}
                    speed="fast"
                    className="text-sm"
                  />
                </div>
              )}
            </div>
          </motion.div>
        ))}
      </AnimatePresence>

      {activities.length === 0 && (
        <div className="text-muted-foreground text-sm italic">
          Waiting for activity...
        </div>
      )}
    </div>
  );
}
