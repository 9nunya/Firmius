'use client';

import React, { useRef, useEffect, useState } from 'react';
import { cn } from '@/lib/utils';

export interface StreamingTextProps {
  content: string;
  isStreaming?: boolean;
  className?: string;
  speed?: 'fast' | 'normal' | 'slow';
}

export function StreamingText({
  content,
  isStreaming = false,
  className,
  speed = 'fast',
}: StreamingTextProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [displayedContent, setDisplayedContent] = useState(content);
  const contentRef = useRef(content);
  const animationRef = useRef<number | null>(null);

  const speedConfig = {
    fast: { charDelay: 8, batchSize: 3 },
    normal: { charDelay: 16, batchSize: 2 },
    slow: { charDelay: 32, batchSize: 1 },
  };

  // Accumulate content rather than replace
  useEffect(() => {
    if (!isStreaming) {
      setDisplayedContent(content);
      contentRef.current = content;
      return;
    }

    const targetContent = content;
    const currentContent = contentRef.current;

    // If content shrunk (rare), just update immediately
    if (targetContent.length < currentContent.length) {
      setDisplayedContent(targetContent);
      contentRef.current = targetContent;
      return;
    }

    // Animate the difference
    const newText = targetContent.slice(currentContent.length);
    if (newText.length === 0) return;

    const { charDelay, batchSize } = speedConfig[speed];
    let index = 0;

    const animate = () => {
      const batch = newText.slice(index, index + batchSize);
      index += batch.length;

      contentRef.current = targetContent.slice(0, currentContent.length + index);
      setDisplayedContent(contentRef.current);

      if (index < newText.length) {
        animationRef.current = window.setTimeout(() => {
          animationRef.current = requestAnimationFrame(animate);
        }, charDelay) as unknown as number;
      }
    };

    animationRef.current = requestAnimationFrame(animate);

    return () => {
      if (animationRef.current) {
        if (typeof animationRef.current === 'number') {
          cancelAnimationFrame(animationRef.current);
        }
        clearTimeout(animationRef.current as unknown as number);
      }
    };
  }, [content, isStreaming, speed]);

  // Auto-scroll as content grows
  useEffect(() => {
    if (containerRef.current && isStreaming) {
      const container = containerRef.current;
      const isAtBottom = container.scrollHeight - container.scrollTop <= container.clientHeight + 100;
      
      if (isAtBottom) {
        container.scrollTop = container.scrollHeight;
      }
    }
  }, [displayedContent, isStreaming]);

  return (
    <div
      ref={containerRef}
      className={cn(
        'whitespace-pre-wrap break-words',
        isStreaming && 'will-change-contents',
        className
      )}
    >
      {displayedContent}
      {isStreaming && (
        <span className="inline-block w-2 h-4 bg-foreground/50 animate-pulse ml-0.5 align-middle" />
      )}
    </div>
  );
}
