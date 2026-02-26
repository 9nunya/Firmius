'use client';

import React, { useCallback, useEffect } from 'react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import { FleetTab } from '@/components/fleet/FleetSidebar';
import { TodosTab } from '@/components/fleet/TodosTab';
import { ChangesTab } from '@/components/fleet/ChangesTab';
import { motion, AnimatePresence } from 'framer-motion';

interface RightSidebarProps {
  isMobile: boolean;
}

export function RightSidebar({ isMobile }: RightSidebarProps) {
  const {
    rightSidebarTab,
    setRightSidebarTab,
    isRightSidebarOpen,
    closeRightSidebar,
    activeThreadId,
  } = useAppStore();

  // ALL HOOKS MUST BE CALLED BEFORE ANY CONDITIONAL RETURNS
  const TabButton = useCallback(({
    tab,
    label,
  }: {
    tab: 'fleet' | 'todos' | 'changes';
    label: string;
  }) => (
    <button
      onClick={() => setRightSidebarTab(tab)}
      className={cn(
        'px-3 py-2 text-xs font-medium transition-colors relative',
        rightSidebarTab === tab
          ? 'text-foreground'
          : 'text-muted-foreground hover:text-foreground/80'
      )}
    >
      {label}
      {rightSidebarTab === tab && (
        <motion.div
          layoutId={isMobile ? "activeTabMobile" : "activeTab"}
          className="absolute bottom-0 left-0 right-0 h-0.5 bg-foreground"
          transition={{ duration: 0.15 }}
        />
      )}
    </button>
  ), [rightSidebarTab, setRightSidebarTab, isMobile]);

  const TabContent = useCallback(() => {
    switch (rightSidebarTab) {
      case 'fleet':
        return <FleetTab />;
      case 'todos':
        return <TodosTab />;
      case 'changes':
        return <ChangesTab />;
      default:
        return <FleetTab />;
    }
  }, [rightSidebarTab]);

  // Close sidebar when pressing Escape (only on mobile)
  useEffect(() => {
    if (!isMobile) return;
    const handleEscape = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && isRightSidebarOpen) {
        closeRightSidebar();
      }
    };
    window.addEventListener('keydown', handleEscape);
    return () => window.removeEventListener('keydown', handleEscape);
  }, [isMobile, isRightSidebarOpen, closeRightSidebar]);

  // Don't render without active thread
  if (!activeThreadId) {
    return null;
  }

  // Desktop view
  if (!isMobile) {
    return (
      <aside
        className="flex flex-col border-l border-border bg-background flex-shrink-0 w-[200px]"
      >
        <div className="flex items-center border-b border-border">
          <TabButton tab="fleet" label="Fleet" />
          <TabButton tab="todos" label="Todos" />
          <TabButton tab="changes" label="Changes" />
        </div>
        <div className="flex-1 overflow-hidden">
          <TabContent />
        </div>
      </aside>
    );
  }

  // Mobile view
  return (
    <div>
      <AnimatePresence>
        {isRightSidebarOpen && (
          <>
            <motion.div
              initial={{ opacity: 0 }}
              animate={{ opacity: 1 }}
              exit={{ opacity: 0 }}
              transition={{ duration: 0.2 }}
              className="fixed inset-0 bg-black/50 z-40"
              onClick={closeRightSidebar}
            />
            <motion.aside
              initial={{ x: '100%' }}
              animate={{ x: 0 }}
              exit={{ x: '100%' }}
              transition={{ duration: 0.3, ease: [0.32, 0.72, 0, 1] }}
              className="fixed right-0 top-0 bottom-0 w-[280px] bg-background z-50 flex flex-col border-l border-border pt-[env(safe-area-inset-top)] pb-[env(safe-area-inset-bottom)]"
            >
              <div className="flex items-center justify-between px-3 py-2 border-b border-border">
                <div className="flex items-center">
                  <TabButton tab="fleet" label="Fleet" />
                  <TabButton tab="todos" label="Todos" />
                  <TabButton tab="changes" label="Changes" />
                </div>
                <button
                  onClick={closeRightSidebar}
                  className="p-2 text-muted-foreground hover:text-foreground transition-colors"
                >
                  <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                    <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
                  </svg>
                </button>
              </div>
              <div className="flex-1 overflow-hidden">
                <TabContent />
              </div>
            </motion.aside>
          </>
        )}
      </AnimatePresence>
    </div>
  );
}
