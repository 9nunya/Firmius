'use client';

import React, { useState, useEffect, Suspense } from 'react';
import { useSearchParams } from 'next/navigation';
import { AppShell } from '@/components/layout/AppShell';
import { Sidebar } from '@/components/layout/Sidebar';
import { ChatPanel } from '@/components/layout/ChatPanel';
import { InputBar } from '@/components/layout/InputBar';
import { StatusBar } from '@/components/layout/StatusBar';
import { MessageList } from '@/components/chat/MessageList';
import { MobileTitleBar } from '@/components/layout/MobileTitleBar';
import { useSSE } from '@/hooks/useSSE';
import { useCommandPalette, type CommandPaletteAction } from '@/hooks/useCommandPalette';
import { CommandPalette } from '@/components/CommandPalette';
import CreateThreadModal from '@/components/modals/CreateThreadModal';
import { RightSidebar } from '@/components/layout/RightSidebar';
import { MobileBottomBar } from '@/components/layout/MobileBottomBar';
import useAppStore from '@/stores/app-store';
import SettingsPanel from '@/components/settings/SettingsPanel';
import { LoadingScreen } from '@/components/ui/LoadingScreen';

function HomeContent() {
  const searchParams = useSearchParams();
  const { activeThreadId, toggleRightSidebar, sidebarTab, selectThread } = useAppStore();
  const [isMobile, setIsMobile] = useState(false);

  useSSE();

  // Detect mobile viewport
  useEffect(() => {
    const checkMobile = () => {
      setIsMobile(window.innerWidth < 768);
    };
    checkMobile();
    window.addEventListener('resize', checkMobile);
    return () => window.removeEventListener('resize', checkMobile);
  }, []);

  const threadId = searchParams.get('thread');

  useEffect(() => {
    if (threadId && threadId !== activeThreadId) {
      selectThread(threadId);
    }
  }, [threadId, activeThreadId, selectThread]);

  const [isInitialized, setIsInitialized] = useState(false);
  const [showCreateModal, setShowCreateModal] = useState(false);
  const [isMobileSidebarOpen, setIsMobileSidebarOpen] = useState(false);

  useEffect(() => {
    setIsInitialized(true);
  }, []);

  const commandPalette = useCommandPalette();

  if (!isInitialized) {
    return <LoadingScreen fullScreen />;
  }

  const commandPaletteActions: CommandPaletteAction[] = [
    {
      id: 'new-thread',
      label: 'New Thread',
      action: () => setShowCreateModal(true),
    },
    {
      id: 'toggle-fleet',
      label: 'Toggle Fleet Sidebar',
      action: () => {
        useAppStore.getState().toggleFleetSidebar();
      },
    },
    {
      id: 'cancel',
      label: 'Cancel',
      action: () => commandPalette.close(),
    },
  ];

  return (
    <AppShell>
      <Sidebar
        isMobileOpen={isMobileSidebarOpen}
        setIsMobileOpen={setIsMobileSidebarOpen}
      />
      <div className="flex flex-1 flex-col overflow-hidden relative">
        <MobileTitleBar
          onToggleSidebar={() => setIsMobileSidebarOpen(!isMobileSidebarOpen)}
          onToggleRightSidebar={toggleRightSidebar}
          className="sticky top-0 z-30 bg-background/95 backdrop-blur-sm"
        />
        {sidebarTab === 'threads' ? (
          <ChatPanel
            className="flex-1"
            onCreateThread={() => setShowCreateModal(true)}
          >
            <MessageList />
          </ChatPanel>
        ) : (
          <div className="flex-1 overflow-auto">
            <SettingsPanel />
          </div>
        )}
        {sidebarTab === 'threads' && activeThreadId && !isMobile && (
          <div className="sticky bottom-0 z-10 bg-card border-t border-border">
            <InputBar />
            <StatusBar />
          </div>
        )}

        {/* Mobile InputBar + StatusBar - fixed above bottom bar */}
        {sidebarTab === 'threads' && activeThreadId && isMobile && (
          <div className="fixed bottom-[60px] left-0 right-0 z-20 bg-card border-t border-border">
            <StatusBar />
            <InputBar />
          </div>
        )}
      </div>

      <RightSidebar isMobile={isMobile} />

      {/* Mobile Bottom Bar */}
      <MobileBottomBar />

      {showCreateModal && (
        <CreateThreadModal isOpen={true} onClose={() => setShowCreateModal(false)} />
      )}
      <CommandPalette
        isOpen={commandPalette.isOpen}
        onClose={commandPalette.close}
        actions={commandPaletteActions}
      />
    </AppShell>
  );
}

export default function Home() {
  return (
    <Suspense fallback={<LoadingScreen fullScreen />}>
      <HomeContent />
    </Suspense>
  );
}