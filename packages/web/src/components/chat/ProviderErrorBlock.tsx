import { useState } from 'react';
import { ChevronDown, ChevronRight, AlertCircle } from 'lucide-react';
import { cn } from '@/lib/utils';

export interface ProviderError {
  error: string;
  modelId?: string;
  providerId?: string;
}

interface ProviderErrorBlockProps {
  error: ProviderError;
}

export default function ProviderErrorBlock({ error }: ProviderErrorBlockProps) {
  const [isExpanded, setIsExpanded] = useState(true);

  const timestamp = new Date().toLocaleTimeString();

  return (
    <div className={cn(
      "mb-2 rounded-lg border border-red-200 bg-red-50 dark:border-red-800 dark:bg-red-900/20",
      "border-l-4 border-l-red-400"
    )}>
      <button
        onClick={() => setIsExpanded(!isExpanded)}
        className="flex w-full items-center gap-2 rounded-t-lg p-3 hover:bg-red-100 dark:hover:bg-red-900/30 transition-colors"
      >
        <AlertCircle size={16} className="text-red-500" />
        
        <span className="px-2 py-0.5 rounded text-xs font-medium bg-red-100 text-red-700 dark:bg-red-800 dark:text-red-300">
          Provider Error
        </span>
        
        <span className="flex-1 text-left text-xs text-red-600 dark:text-red-400">
          {timestamp}
        </span>
        
        {isExpanded ? (
          <ChevronDown size={16} className="text-red-400" />
        ) : (
          <ChevronRight size={16} className="text-red-400" />
        )}
      </button>

      {isExpanded && (
        <div className="border-t border-red-200 dark:border-red-800">
          <div className="p-3 space-y-3">
            <div>
              <div className="mb-1 text-xs font-medium text-red-600 dark:text-red-400">Error Message</div>
              <pre className="overflow-x-auto rounded bg-red-100 p-2 text-xs font-mono text-red-800 dark:bg-red-900/50 dark:text-red-200">
                {error.error}
              </pre>
            </div>

            {(error.modelId || error.providerId) && (
              <div className="flex gap-4 text-xs">
                {error.modelId && (
                  <div>
                    <span className="font-medium text-red-600 dark:text-red-400">Model: </span>
                    <span className="text-red-700 dark:text-red-300">{error.modelId}</span>
                  </div>
                )}
                {error.providerId && (
                  <div>
                    <span className="font-medium text-red-600 dark:text-red-400">Provider: </span>
                    <span className="text-red-700 dark:text-red-300">{error.providerId}</span>
                  </div>
                )}
              </div>
            )}
          </div>
        </div>
      )}
    </div>
  );
}
