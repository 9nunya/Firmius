import { useEffect, useRef } from 'react';

/**
 * Hook that attaches a ref to an element and automatically scrolls it into view
 * when the status transitions from 'running' to any other state (done/error).
 *
 * @param status - The current status of the tool block
 * @returns A ref object to attach to the scrollable element
 */
export function useAutoScroll(status: 'preparing' | 'running' | 'done' | 'error') {
  const ref = useRef<HTMLDivElement>(null);
  const prevStatusRef = useRef<'preparing' | 'running' | 'done' | 'error'>(status);

  useEffect(() => {
    // Only scroll if we just finished (transition from running to non-running)
    if (prevStatusRef.current === 'running' && status !== 'running') {
      // Use a short timeout to ensure DOM has updated with the new content
      const timer = setTimeout(() => {
        ref.current?.scrollIntoView({
          behavior: 'smooth',
          block: 'nearest',
          inline: 'nearest',
        });
      }, 50);
      return () => clearTimeout(timer);
    }
    prevStatusRef.current = status;
  }, [status]);

  return ref;
}
