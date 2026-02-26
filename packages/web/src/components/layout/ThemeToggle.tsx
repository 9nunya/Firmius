'use client';

import * as React from 'react';
import { Moon, Sun, Laptop } from 'lucide-react';
import { useTheme } from 'next-themes';
import { cn } from '@/lib/utils';

export function ThemeToggle({ className }: { className?: string }) {
  const { theme, setTheme } = useTheme();
  const [mounted, setMounted] = React.useState(false);

  // useEffect only runs on the client, so now we can safely show the UI
  React.useEffect(() => {
    setMounted(true);
  }, []);

  if (!mounted) {
    return null;
  }

  return (
    <div className={cn("flex items-center gap-1 p-1 bg-muted/50 rounded-lg border border-border/50", className)}>
      <button
        type="button"
        onClick={() => setTheme('light')}
        className={cn(
          "p-1.5 rounded-md transition-all",
          theme === 'light' 
            ? "bg-background text-foreground shadow-sm" 
            : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}
        title="Light Mode"
      >
        <Sun size={14} />
      </button>
      <button
        type="button"
        onClick={() => setTheme('system')}
        className={cn(
          "p-1.5 rounded-md transition-all",
          theme === 'system' 
            ? "bg-background text-foreground shadow-sm" 
            : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}
        title="System Mode"
      >
        <Laptop size={14} />
      </button>
      <button
        type="button"
        onClick={() => setTheme('dark')}
        className={cn(
          "p-1.5 rounded-md transition-all",
          theme === 'dark' 
            ? "bg-background text-foreground shadow-sm" 
            : "text-muted-foreground hover:text-foreground hover:bg-muted"
        )}
        title="Dark Mode"
      >
        <Moon size={14} />
      </button>
    </div>
  );
}
