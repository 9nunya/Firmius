'use client';

import React, { useState, useEffect, useRef } from 'react';
import { Search, X } from 'lucide-react';
import { cn } from '@/lib/utils';
import type { CommandPaletteAction } from '@/hooks/useCommandPalette';

interface CommandPaletteProps {
  actions: CommandPaletteAction[];
  isOpen: boolean;
  onClose: () => void;
}

export function CommandPalette({ actions, isOpen, onClose }: CommandPaletteProps): React.ReactElement | null {
  const [search, setSearch] = useState('');
  const [selectedIndex, setSelectedIndex] = useState(0);

  const inputRef = useRef<HTMLInputElement>(null);
  const listRef = useRef<HTMLUListElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);

  // Filter actions based on search
  const filteredActions = actions.filter(action =>
    action.label.toLowerCase().includes(search.toLowerCase())
  );

  // Reset state when opening
  useEffect(() => {
    if (isOpen) {
      setSearch('');
      setSelectedIndex(0);
      // Focus input after modal renders
      setTimeout(() => inputRef.current?.focus(), 0);
    }
  }, [isOpen]);

  // Reset selected index when filtered list changes
  useEffect(() => {
    setSelectedIndex(0);
  }, [search, filteredActions.length]);

  // Keyboard navigation and shortcuts
  useEffect(() => {
    if (!isOpen) return;

    const handleKeyDown = (e: KeyboardEvent) => {
      switch (e.key) {
        case 'Escape':
          e.preventDefault();
          onClose();
          break;
        case 'ArrowDown':
          e.preventDefault();
          setSelectedIndex(prev => Math.min(prev + 1, filteredActions.length - 1));
          break;
        case 'ArrowUp':
          e.preventDefault();
          setSelectedIndex(prev => Math.max(prev - 1, 0));
          break;
        case 'Enter':
          e.preventDefault();
          if (filteredActions[selectedIndex]) {
            filteredActions[selectedIndex].action();
            onClose();
          }
          break;
        default:
          break;
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [isOpen, filteredActions, selectedIndex, onClose]);

  // Click outside to close
  useEffect(() => {
    if (!isOpen) return;

    const handleClickOutside = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        onClose();
      }
    };

    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, [isOpen, onClose]);

  // Ensure selected item is in view
  useEffect(() => {
    if (!isOpen || !listRef.current) return;

    const selectedElement = listRef.current.querySelector(`[data-index="${selectedIndex}"]`);
    if (selectedElement) {
      selectedElement.scrollIntoView({ block: 'nearest' });
    }
  }, [selectedIndex, isOpen]);

  if (!isOpen) return null;

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/50 p-4"
      ref={containerRef}
    >
      <div className="flex w-full max-w-2xl flex-col rounded-lg border border-border bg-card shadow-xl">
        {/* Search Header */}
        <div className="flex items-center border-b border-border px-4 py-3">
          <Search size={20} className="mr-3 text-muted-foreground" />
          <input
            ref={inputRef}
            type="text"
            value={search}
            onChange={(e) => setSearch(e.target.value)}
            placeholder="Type a command..."
             className="flex-1 bg-transparent outline-none placeholder:text-muted-foreground"
          />
          <button
            type="button"
            onClick={onClose}
            className="rounded-md p-2 text-muted-foreground hover:bg-accent hover:text-accent-foreground"
          >
            <X size={20} />
          </button>
        </div>

        {/* Actions List */}
        <ul ref={listRef} className="max-h-80 overflow-y-auto p-2">
          {filteredActions.length === 0 ? (
            <li className="px-4 py-3 text-sm text-muted-foreground">No commands found</li>
          ) : (
            filteredActions.map((action, index) => (
              <li
                key={action.id}
                data-index={index}
                onClick={() => {
                  action.action();
                  onClose();
                }}
                className={cn(
                  'cursor-pointer rounded-md px-4 py-3 text-sm transition-colors',
                  index === selectedIndex
                    ? 'bg-accent text-accent-foreground'
                    : 'hover:bg-accent/50'
                )}
              >
                <span>{action.label}</span>
                {action.shortcut && (
                  <kbd className="ml-auto rounded bg-muted px-2 py-0.5 text-xs font-mono text-muted-foreground">
                    {action.shortcut}
                  </kbd>
                )}
              </li>
            ))
          )}
        </ul>

        {/* Keyboard Shortcuts Help */}
        <div className="border-t border-border px-4 py-2 text-xs text-muted-foreground bg-muted/30">
          <div className="flex flex-wrap gap-x-4 gap-y-1">
            <span className="flex items-center gap-1">
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">Ctrl</kbd>
              <span>+</span>
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">M</kbd>
              <span>Palette</span>
            </span>
            <span className="flex items-center gap-1">
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">↵</kbd>
              <span>Send</span>
            </span>
            <span className="flex items-center gap-1">
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">Shift</kbd>
              <span>+</span>
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">↵</kbd>
              <span>New Line</span>
            </span>
            <span className="flex items-center gap-1">
              <kbd className="rounded bg-background border border-border px-1.5 py-0.5 text-[10px] font-mono">Esc</kbd>
              <span>Close</span>
            </span>
          </div>
        </div>
      </div>
    </div>
  );
}

export default CommandPalette;
