'use client';

import React, { useEffect, useState, useMemo, useRef } from 'react';
import useAppStore from '@/stores/app-store';
import { 
  Cpu, 
  Zap, 
  Loader2, 
  Search, 
  Eye, 
  Mic,
  Check,
  ChevronDown,
  Settings2
} from 'lucide-react';
import { motion, AnimatePresence } from 'framer-motion';

/**
 * ModelSwitcher Overhaul 2.0
 * A high-fidelity, IDE-grade model selector with command palette search,
 * provider categorization, and advanced modality badges.
 */
export const ModelSwitcher: React.FC = () => {
  const { providers, loadProviders, updateThreadSettings, threads, activeThreadId } = useAppStore();
  const [isOpen, setIsOpen] = useState(false);
  const [searchTerm, setSearchTerm] = useState('');
  const searchInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    loadProviders();
  }, [loadProviders]);

  // Focus input when dropdown opens
  useEffect(() => {
    if (isOpen) {
      setTimeout(() => searchInputRef.current?.focus(), 100);
    }
  }, [isOpen]);

  const currentThread = threads.find(t => t.id === activeThreadId);
  const currentModelId = currentThread?.modelId;

  const currentModel = useMemo(() => {
    for (const p of providers) {
      const model = p.models.find(m => m.id === currentModelId);
      if (model) return model;
    }
    return null;
  }, [providers, currentModelId]);

  const filteredProviders = useMemo(() => {
    return providers.map(p => ({
      ...p,
      models: p.models.filter(m => 
        m.name.toLowerCase().includes(searchTerm.toLowerCase()) ||
        p.name.toLowerCase().includes(searchTerm.toLowerCase())
      )
    })).filter(p => p.models.length > 0);
  }, [providers, searchTerm]);

  const handleModelSelect = async (providerId: string, modelId: string) => {
    await updateThreadSettings({ providerId, modelId });
    setIsOpen(false);
  };

  const handleEffortChange = async (effort: string) => {
    await updateThreadSettings({ reasoningEffort: effort });
  };

  if (!currentThread) return null;

  const formatCtx = (ctx: number) => {
    if (ctx >= 1000000) return `${(ctx / 1000000).toFixed(0)}M`;
    if (ctx >= 1000) return `${(ctx / 1000).toFixed(0)}k`;
    return ctx.toString();
  };

  return (
    <div className="relative">
      {/* Trigger Button */}
      <motion.button
        whileTap={{ scale: 0.98 }}
        onClick={() => setIsOpen(!isOpen)}
        className={`
          group flex items-center gap-3 px-3 py-2 text-xs font-medium transition-all duration-200
          bg-card border rounded-lg shadow-sm
          ${isOpen 
            ? 'border-indigo-500 shadow-indigo-500/10 ring-1 ring-indigo-500/20' 
            : 'border-border hover:border-indigo-500/30 hover:bg-muted/10'
          }
          disabled:opacity-50 disabled:cursor-not-allowed
        `}
      >
        <div className={`
          flex items-center justify-center w-6 h-6 rounded-md transition-colors
          ${isOpen ? 'bg-indigo-500/20 text-indigo-400' : 'bg-muted text-muted-foreground group-hover:text-indigo-400'}
        `}>
          {!isOpen && providers.length === 0 ? (
            <Loader2 size={14} className="animate-spin" />
          ) : (
            <Cpu size={14} />
          )}
        </div>
        
        <div className="flex flex-col items-start leading-tight">
          <span className="text-[9px] text-muted-foreground font-bold uppercase tracking-wider mb-0.5 opacity-60">Engine</span>
          <span className={`truncate max-w-[120px] font-mono ${isOpen ? 'text-foreground' : 'text-foreground/80 group-hover:text-foreground'}`}>
            {currentModel?.name || currentModelId || 'Select Model'}
          </span>
        </div>

        <div className="ml-auto flex items-center gap-2">
          <ChevronDown 
            size={14} 
            className={`text-muted-foreground transition-transform duration-300 ${isOpen ? 'rotate-180 text-indigo-400' : 'group-hover:text-indigo-400'}`} 
          />
        </div>
      </motion.button>

      {/* Dropdown Menu */}
      <AnimatePresence>
        {isOpen && (
          <>
            <div className="fixed inset-0 z-[60]" onClick={() => setIsOpen(false)} />
            <motion.div
              initial={{ opacity: 0, y: -8, scale: 0.96 }}
              animate={{ opacity: 1, y: 0, scale: 1 }}
              exit={{ opacity: 0, y: -8, scale: 0.96 }}
              transition={{ duration: 0.15, ease: [0.23, 1, 0.32, 1] }}
              className="absolute top-full left-0 mt-3 w-[360px] bg-card border border-border rounded-xl shadow-2xl z-[70] overflow-hidden flex flex-col max-h-[540px] backdrop-blur-xl origin-top-left"
            >
              {/* Search Header (Command Palette Style) */}
              <div className="relative p-3 border-b border-border bg-muted/20">
                <Search size={14} className="absolute left-6 top-1/2 -translate-y-1/2 text-muted-foreground" />
                <input
                  ref={searchInputRef}
                  type="text"
                  placeholder="Search providers and models..."
                  value={searchTerm}
                  onChange={(e) => setSearchTerm(e.target.value)}
                  className="w-full bg-background border border-border rounded-lg pl-10 pr-10 py-2.5 text-xs text-foreground placeholder:text-muted-foreground focus:outline-none focus:border-indigo-500/50 focus:ring-1 focus:ring-indigo-500/20 transition-all font-mono"
                />
              </div>

              {/* Scrollable Content */}
              <div className="flex-1 overflow-y-auto custom-scrollbar p-2 space-y-2 min-h-0">
                {filteredProviders.length === 0 ? (
                  <div className="py-12 flex flex-col items-center justify-center gap-3 text-muted-foreground">
                    <div className="p-3 rounded-full bg-muted/50 border border-border">
                      <Search size={20} className="opacity-20" />
                    </div>
                    <span className="text-xs">No matching models discovered</span>
                  </div>
                ) : (
                  filteredProviders.map(provider => (
                    <div key={provider.id} className="mb-3">
                      <div className="flex items-center gap-2 px-3 py-2 mb-1">
                        <div className="w-1 h-3 rounded-full bg-indigo-500/60" />
                        <span className="text-[10px] font-black text-muted-foreground uppercase tracking-[0.2em]">{provider.name}</span>
                      </div>
                      
                      <div className="space-y-1">
                        {provider.models.map(model => {
                          const isSelected = currentModelId === model.id;
                          return (
                            <motion.button
                              layout
                              key={model.id}
                              onClick={() => handleModelSelect(provider.id, model.id)}
                              className={`
                                w-full text-left p-3 rounded-lg text-xs transition-all duration-200 group relative border
                                ${isSelected 
                                  ? 'bg-indigo-500/5 text-foreground border-indigo-500/30' 
                                  : 'text-muted-foreground hover:bg-muted/50 border-transparent hover:border-border/50'
                                }
                              `}
                            >
                              <div className="flex justify-between items-start mb-2">
                                <span className={`font-mono font-bold tracking-tight ${isSelected ? 'text-indigo-400' : 'group-hover:text-foreground'}`}>
                                  {model.name}
                                </span>
                                {isSelected && (
                                  <motion.div layoutId="active-check">
                                    <Check size={14} className="text-indigo-500" />
                                  </motion.div>
                                )}
                              </div>
                              
                              <div className="flex flex-wrap items-center gap-1.5">
                                {/* Context Window Badge */}
                                <span className="px-1.5 py-0.5 bg-background border border-border rounded text-[9px] font-mono font-bold text-muted-foreground">
                                  {formatCtx(model.ctx)}
                                </span>

                                {/* Capability Badges */}
                                {model.reasoning && (
                                  <span className="flex items-center gap-1 px-1.5 py-0.5 bg-amber-500/10 border border-amber-500/20 rounded text-[9px] text-amber-600 dark:text-amber-500 font-black">
                                    <Zap size={9} fill="currentColor" className="text-amber-500" />
                                    R1
                                  </span>
                                )}
                                {model.capabilities?.vision && (
                                  <span className="flex items-center gap-1 px-1.5 py-0.5 bg-blue-500/10 border border-blue-500/20 rounded text-[9px] text-blue-600 dark:text-blue-500 font-black">
                                    <Eye size={9} />
                                    VIS
                                  </span>
                                )}
                                {model.modalities?.input?.includes('audio') && (
                                  <span className="flex items-center gap-1 px-1.5 py-0.5 bg-rose-500/10 border border-rose-500/20 rounded text-[9px] text-rose-600 dark:text-rose-500 font-black">
                                    <Mic size={9} />
                                    AUD
                                  </span>
                                )}
                              </div>
                            </motion.button>
                          );
                        })}
                      </div>
                    </div>
                  ))
                )}
              </div>

              {/* Thread Settings Footer (Contextual) */}
              {currentModel?.reasoning && (
                <div className="p-4 border-t border-border bg-muted/20">
                  <div className="flex items-center justify-between mb-3">
                    <div className="flex items-center gap-2">
                      <div className="p-1 rounded bg-amber-500/10 border border-amber-500/20">
                        <Zap size={10} className="text-amber-500" />
                      </div>
                      <span className="text-[10px] font-black text-muted-foreground uppercase tracking-widest opacity-70">Reasoning Effort</span>
                    </div>
                    <Settings2 size={12} className="text-muted-foreground opacity-40" />
                  </div>
                  
                  <div className="grid grid-cols-5 bg-background border border-border p-1 rounded-lg gap-1">
                    {['none', 'minimal', 'low', 'medium', 'high'].map((effort) => {
                      const isActive = currentThread?.reasoningEffort === effort;
                      return (
                        <button
                          key={effort}
                          onClick={() => handleEffortChange(effort)}
                          className={`
                            relative py-1.5 rounded-md text-[9px] font-black uppercase transition-all duration-200
                            ${isActive
                              ? 'text-white'
                              : 'text-muted-foreground hover:text-foreground'
                            }
                          `}
                        >
                          {isActive && (
                            <motion.div
                              layoutId="active-effort"
                              className="absolute inset-0 bg-indigo-500 rounded-md shadow-[0_0_15px_rgba(99,102,241,0.3)]"
                              style={{ zIndex: 0 }}
                            />
                          )}
                          <span className="relative z-10">{effort === 'none' ? 'Off' : effort.slice(0, 3)}</span>
                        </button>
                      );
                    })}
                  </div>
                </div>
              )}
            </motion.div>
          </>
        )}
      </AnimatePresence>
    </div>
  );
};
