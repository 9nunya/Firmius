'use client';

import React, { useState, useMemo } from 'react';
import { Search, ChevronDown, Check } from 'lucide-react';
import { cn } from '@/lib/utils';
import type { ProviderInfo, ModelInfo } from '@/types';

interface ModelEntry {
  providerId: string;
  providerName: string;
  model: ModelInfo;
}

interface ModelSelectorProps {
  providers: ProviderInfo[];
  value: { providerId: string; modelId: string };
  onChange: (providerId: string, modelId: string) => void;
  className?: string;
}

export function ModelSelector({ providers, value, onChange, className }: ModelSelectorProps) {
  const [open, setOpen] = useState(false);
  const [search, setSearch] = useState('');
  const containerRef = React.useRef<HTMLDivElement>(null);

  // Close on outside click
  React.useEffect(() => {
    if (!open) return;
    const handleClickOutside = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        setOpen(false);
      }
    };
    const handleEscape = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false);
    };
    document.addEventListener('mousedown', handleClickOutside);
    document.addEventListener('keydown', handleEscape);
    return () => {
      document.removeEventListener('mousedown', handleClickOutside);
      document.removeEventListener('keydown', handleEscape);
    };
  }, [open]);

  // Flatten all models with provider reference
  const allModels = useMemo<ModelEntry[]>(() => {
    return providers.flatMap((provider) =>
      provider.models.map((model: ModelInfo) => ({
        providerId: provider.id,
        providerName: provider.name,
        model,
      }))
    );
  }, [providers]);

  // Filter by search
  const filtered = useMemo(() => {
    if (!search) return allModels;
    const q = search.toLowerCase();
    return allModels.filter(
      m =>
        m.model.name.toLowerCase().includes(q) ||
        m.providerName.toLowerCase().includes(q) ||
        m.model.id.toLowerCase().includes(q)
    );
  }, [allModels, search]);

  const selectedEntry = useMemo(() => {
    return allModels.find(m => m.providerId === value.providerId && m.model.id === value.modelId);
  }, [allModels, value]);

  const handleSelect = (entry: ModelEntry) => {
    onChange(entry.providerId, entry.model.id);
    setOpen(false);
    setSearch('');
  };

  return (
    <div ref={containerRef} className={cn('relative', className)}>
      {/* Trigger */}
      <button
        type="button"
        onClick={() => setOpen(true)}
        className={cn(
          'flex h-10 w-full items-center justify-between rounded-md border border-input bg-background px-3 py-2 text-sm ring-offset-background placeholder:text-muted-foreground focus:outline-none focus:ring-2 focus:ring-ring focus:ring-offset-2 disabled:cursor-not-allowed disabled:opacity-50'
        )}
      >
        <div className='flex items-center gap-2 truncate'>
          {selectedEntry ? (
            <>
              <span className='text-muted-foreground text-xs'>{selectedEntry.providerName}</span>
              <span className='mx-1 text-muted-foreground'>/</span>
              <span className='truncate'>{selectedEntry.model.name}</span>
            </>
          ) : (
            <span className='text-muted-foreground'>Select model...</span>
          )}
        </div>
        <ChevronDown size={16} className='text-muted-foreground' />
      </button>

      {/* Dropdown */}
      {open && (
        <div className='absolute z-50 mt-1 w-full overflow-hidden rounded-md border bg-popover text-popover-foreground shadow-lg'>
          {/* Search */}
          <div className='flex items-center border-b px-2'>
            <Search size={14} className='mr-2 text-muted-foreground' />
            <input
              autoFocus
              type='text'
              placeholder='Search models...'
              value={search}
              onChange={e => setSearch(e.target.value)}
              className='flex-1 bg-transparent py-2 text-sm outline-none'
            />
          </div>

          {/* List */}
          <div className='max-h-80 overflow-auto p-1'>
            {filtered.length === 0 ? (
              <div className='py-4 text-center text-sm text-muted-foreground'>No models found</div>
            ) : (
              filtered.map(entry => {
                const isSelected = value.providerId === entry.providerId && value.modelId === entry.model.id;
                const supportsReasoning = entry.model.reasoning?.supported;
                return (
                  <div
                    key={`${entry.providerId}/${entry.model.id}`}
                    onClick={() => handleSelect(entry)}
                    className={cn(
                      'flex cursor-pointer items-center justify-between rounded-sm px-2 py-2 text-sm',
                      isSelected ? 'bg-accent text-accent-foreground' : 'hover:bg-accent/50'
                    )}
                  >
                    <div className='flex flex-col truncate'>
                      <div className='flex items-center gap-2'>
                        <span className='truncate font-medium'>{entry.model.name}</span>
                        {supportsReasoning && (
                          <span className='rounded bg-blue-900 px-1 text-[10px] text-blue-200'>reasoning</span>
                        )}
                      </div>
                      <div className='text-xs text-muted-foreground'>{entry.providerName}</div>
                    </div>
                    {isSelected && <Check size={16} />}
                  </div>
                );
              })
            )}
          </div>
        </div>
      )}
    </div>
  );
}
