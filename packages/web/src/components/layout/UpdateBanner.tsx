'use client';

import { useServiceWorker } from '@/lib/sw';
import { Button } from '@/components/ui/button';
import { X, RefreshCw } from 'lucide-react';
import { useState } from 'react';

export function UpdateBanner() {
  const { updateAvailable, clearCacheAndReload } = useServiceWorker();
  const [dismissed, setDismissed] = useState(false);

  if (!updateAvailable || dismissed) {
    return null;
  }

  return (
    <div className="fixed bottom-4 left-4 right-4 md:left-auto md:right-4 md:w-auto z-50 bg-primary text-primary-foreground px-4 py-3 rounded-lg shadow-lg flex items-center gap-3 animate-in slide-in-from-bottom-4">
      <span className="text-sm font-medium">Update available</span>
      <Button
        size="sm"
        variant="secondary"
        onClick={clearCacheAndReload}
        className="gap-1"
      >
        <RefreshCw className="w-4 h-4" />
        Reload
      </Button>
      <Button
        size="icon"
        variant="ghost"
        className="h-8 w-8"
        onClick={() => setDismissed(true)}
      >
        <X className="w-4 h-4" />
      </Button>
    </div>
  );
}
