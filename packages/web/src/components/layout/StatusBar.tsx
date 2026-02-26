'use client';

import React, { useEffect, useMemo, useState } from 'react';
import useAppStore from '@/stores/app-store';
import { sseClient, type SSEStatus } from '@firmius/shared/sse';
import { cn } from '@/lib/utils';
import type { Agent } from '@firmius/shared/api';
import { Loader2 } from 'lucide-react';

interface StatusBarProps {
  className?: string;
}

export function StatusBar({ className }: StatusBarProps): React.ReactElement {
  const { threads, activeThreadId, agents, activeAgentId, providers, updateAgentModel, loadProviders, updateThreadSettings } = useAppStore();
  const [sseStatus, setSseStatus] = useState<SSEStatus>('disconnected');
  const [isModelMenuOpen, setIsModelMenuOpen] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const modelMenuRef = React.useRef<HTMLDivElement>(null);
  const searchInputRef = React.useRef<HTMLInputElement>(null);

  // Close model menu when clicking outside
  useEffect(() => {
    const handleClickOutside = (event: MouseEvent) => {
      if (modelMenuRef.current && !modelMenuRef.current.contains(event.target as Node)) {
        setIsModelMenuOpen(false);
      }
    };

    if (isModelMenuOpen) {
      document.addEventListener('mousedown', handleClickOutside);
      // Auto-focus search input when menu opens
      setTimeout(() => searchInputRef.current?.focus(), 10);
    } else {
      setSearchQuery(''); // Reset search when closed
    }

    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
    };
  }, [isModelMenuOpen]);

  // Sync SSE status
  useEffect(() => {
    sseClient.onStatusChange(setSseStatus);
  }, []);

  // Ensure providers are loaded
  useEffect(() => {
    if (providers.length === 0) {
      loadProviders();
    }
  }, [providers.length, loadProviders]);

  // Get active thread
  const activeThread = threads.find(t => t.id === activeThreadId);

  // Get display agent (focused subagent, or lead)
  const displayAgent = useMemo(() => {
    // If user focused a specific agent, show that
    if (activeAgentId) {
      const agent = agents.find((a: Agent) => a.id === activeAgentId);
      if (agent) return agent;
    }

    // Otherwise show lead agent
    if (activeThreadId) {
      const thread = threads.find(t => t.id === activeThreadId);
      if (thread?.leadAgentId) {
        return agents.find(a => a.id === thread.leadAgentId);
      }
    }

    return null;
  }, [activeAgentId, activeThreadId, threads, agents]);

  // Check if any agent is working
  const workingAgent = useMemo(() => {
    return agents.find((a: Agent) => a.status === 'working');
  }, [agents]);

  const isGenerating = workingAgent !== undefined;

  // Get current model context limit based on focused agent
  const currentModelCtx = useMemo(() => {
    const agent = displayAgent;
    if (!agent || !agent.modelId) return null;

    const modelId = agent.modelId;
    const cleanId = modelId.split('/').pop()?.split(':').shift() || modelId;

    for (const p of providers) {
      // Try strict match first
      let m = p.models.find(m => m.id === modelId);
      // Then try clean ID match
      if (!m) m = p.models.find(m => m.id === cleanId);
      // Then try fuzzy match
      if (!m) m = p.models.find(m => modelId.toLowerCase().includes(m.id.toLowerCase()) || m.id.toLowerCase().includes(cleanId.toLowerCase()));

      if (m) return m.ctx;
    }

    // Default catch-all if we can't find it but it's a known model name pattern
    if (cleanId.includes('gpt-4')) return 128000;
    if (cleanId.includes('claude-3')) return 200000;
    if (cleanId.includes('gemini-1.5')) return 1000000;

    return 32000; // Generic fallback
  }, [displayAgent, providers]);

  // Get all available models from all providers
  const availableModels = useMemo(() => {
    // Flatten all models from all providers, grouped by provider
    const allModels: Array<{ providerId: string; providerName: string; model: { id: string; ctx: number; capabilities?: { reasoning?: boolean } } }> = [];

    for (const provider of providers) {
      for (const model of provider.models) {
        allModels.push({
          providerId: provider.id,
          providerName: provider.name || provider.id,
          model
        });
      }
    }

    const filtered = allModels.filter(m => {
      const searchLower = searchQuery.toLowerCase();
      const terms = searchLower.split(/\s+/).filter(Boolean);
      const targetStr = `${m.model.id} ${m.providerName} ${m.providerId}`.toLowerCase();

      return terms.every(term => targetStr.includes(term));
    });

    return filtered;
  }, [providers, searchQuery]);

  // Handle model switch
  const handleModelChange = async (modelId: string, providerId?: string) => {
    if (!activeThreadId) return;

    try {
      if (displayAgent) {
        // Call API to update agent model (and provider if changed)
        await fetch(`/api/threads/${activeThreadId}/agents/${displayAgent.id}/model`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ modelId, providerId }),
        });
        // Update local store
        updateAgentModel(displayAgent.id, modelId);
      } else {
        // Fallback to thread settings
        updateThreadSettings({ modelId, providerId });
      }
      setIsModelMenuOpen(false);
    } catch (err) {
      console.error('Failed to update model:', err);
    }
  };

  return (
    <div
      className={cn(
        'relative flex items-center justify-between border-t border-border bg-background px-3 py-2 text-sm',
        className
      )}
    >
      {/* Left: Connection + Model */}
      <div className="flex items-center gap-3">
        {/* Connection status */}
        <div className="flex items-center gap-1.5" title={`SSE: ${sseStatus}`}>
          <span
            className={cn(
              'w-1.5 h-1.5 rounded-full',
              sseStatus === 'connected' ? 'bg-green-500' :
                sseStatus === 'connecting' ? 'bg-amber-500 animate-pulse' :
                  'bg-red-500'
            )}
          />
          <span className="text-muted-foreground text-xs hidden sm:inline">{sseStatus}</span>
        </div>

        {/* Model selector */}
        {(displayAgent || activeThread) && availableModels.length > 0 && (
          <div className="relative" ref={modelMenuRef}>
            <button
              onClick={() => setIsModelMenuOpen(!isModelMenuOpen)}
              className="flex items-center gap-1.5 text-xs hover:bg-accent/50 px-2 py-1 transition-colors"
              disabled={isGenerating}
            >
              <span className="text-muted-foreground hidden sm:inline">Model:</span>
              <span className="font-mono max-w-[80px] sm:max-w-[none] truncate">{displayAgent?.modelId || activeThread?.modelId}</span>
              <svg className="w-3 h-3 text-muted-foreground flex-shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M5 15l7-7 7 7" />
              </svg>
            </button>

            {/* Model dropdown - positioned bottom on desktop, but since StatusBar is at bottom, it should open UP */}
            {isModelMenuOpen && (
              <div className="absolute bottom-full left-0 mb-1 w-64 bg-background border border-border shadow-lg py-1 z-[100] flex flex-col max-h-[70vh] sm:max-h-80">
                {/* Search Input */}
                <div className="px-2 py-2 border-b border-border sticky top-0 bg-background z-10">
                  <input
                    ref={searchInputRef}
                    type="text"
                    placeholder="Search models..."
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                    className="w-full bg-accent/50 border border-border px-2 py-1 text-xs focus:outline-none focus:ring-1 focus:ring-ring"
                    onKeyDown={(e) => {
                      if (e.key === 'Escape') setIsModelMenuOpen(false);
                    }}
                  />
                </div>

                <div className="overflow-y-auto flex-1">
                  {availableModels.length > 0 ? (
                    availableModels.map(({ providerId, providerName, model }) => (
                      <button
                        key={`${providerId}-${model.id}`}
                        onClick={() => handleModelChange(model.id, providerId)}
                        className={cn(
                          'w-full text-left px-3 py-1.5 text-xs hover:bg-accent/50 transition-colors',
                          (displayAgent?.modelId || activeThread?.modelId) === model.id && 'bg-accent'
                        )}
                      >
                        <div className="font-medium">{model.id}</div>
                        <div className="text-muted-foreground text-[10px]">
                          {providerName} • {model.ctx.toLocaleString()} ctx
                        </div>
                      </button>
                    ))
                  ) : (
                    <div className="px-3 py-4 text-center text-xs text-muted-foreground">
                      No models found
                    </div>
                  )}
                </div>
              </div>
            )}
          </div>
        )}
      </div>

      {/* Center: Working status */}
      <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 pointer-events-none">
        {isGenerating && workingAgent && (
          <div className="flex items-center gap-2 text-xs bg-background/80 px-2 py-1 rounded-md backdrop-blur-sm">
            <Loader2 size={12} className="animate-spin text-amber-500" />
            <span className="text-muted-foreground whitespace-nowrap hidden sm:inline">
              {workingAgent.readableName} working...
            </span>
            <span className="text-muted-foreground whitespace-nowrap sm:hidden">
              working...
            </span>
          </div>
        )}
      </div>

      {/* Right: Context usage + Agent status */}
      <div className="flex items-center gap-2 sm:gap-3">
        {/* Context bar - visible on all screen sizes */}
        {displayAgent && (
          <div className="flex items-center gap-2">
            <span className="text-[10px] sm:text-xs text-muted-foreground/60 font-medium hidden sm:inline tracking-wider">CTX</span>
            <div className="w-10 sm:w-16 h-1 bg-muted/40 overflow-hidden rounded-full">
              <div
                className={cn(
                  'h-full transition-all duration-500 rounded-full',
                  ((displayAgent.tokensUsed || 0) / (currentModelCtx || 32000)) > 0.8 ? 'bg-red-500/80 shadow-[0_0_8px_rgba(239,68,68,0.4)]' : 'bg-indigo-500/60 shadow-[0_0_6px_rgba(99,102,241,0.2)]'
                )}
                style={{ width: `${Math.min(100, Math.max(2, ((displayAgent.tokensUsed || 0) / (currentModelCtx || 32000)) * 100))}%` }}
              />
            </div>
            <span className="text-[10px] font-mono text-muted-foreground/80 flex items-baseline gap-0.5">
              <span>{((displayAgent.tokensUsed || 0) / 1000).toFixed(1)}k</span>
              <span className="opacity-30">/</span>
              <span className="opacity-50">{(currentModelCtx ? currentModelCtx / 1000 : 32).toFixed(0)}k</span>
            </span>
          </div>
        )}

        {/* Agent status */}
        {displayAgent ? (
          <div className="flex items-center gap-1.5 text-xs">
            <span className="text-muted-foreground hidden sm:inline">Status:</span>
            <span className={cn(
              'capitalize',
              displayAgent.status === 'working' && 'text-amber-500',
              displayAgent.status === 'idle' && 'text-green-500'
            )}>
              {displayAgent.status}
            </span>
          </div>
        ) : (
          <span className="text-xs text-muted-foreground hidden sm:inline">No agent</span>
        )}
      </div>
    </div>
  );
}

export default StatusBar;
