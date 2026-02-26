'use client';

import { useEffect, useState, useCallback } from 'react';

export interface CommandPaletteAction {
  id: string;
  label: string;
  shortcut?: string;
  action: () => void;
}

export function useCommandPalette() {
  const [isOpen, setIsOpen] = useState(false);

  const open = useCallback(() => setIsOpen(true), []);
  const close = useCallback(() => setIsOpen(false), []);
  const toggle = useCallback(() => setIsOpen(prev => !prev), []);

  useEffect(() => {
     const handleKeyDown = (event: KeyboardEvent) => {
       // Check for Ctrl+M or Cmd+M
       const isCtrlOrCmd = event.ctrlKey || event.metaKey;
       const isM = event.key?.toLowerCase() === 'm';

       if (isCtrlOrCmd && isM) {
         event.preventDefault();
         event.stopPropagation();
         toggle();
       }
     };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [toggle]);

  return { isOpen, open, close, toggle };
}
