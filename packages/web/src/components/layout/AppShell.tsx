'use client';

import React from 'react';
import { cn } from '@/lib/utils';

interface AppShellProps extends React.PropsWithChildren {
  className?: string;
}

export function AppShell({ children, className }: AppShellProps): React.ReactElement {
  return (
    <div className={cn('flex h-screen w-full bg-background text-foreground', className)}>
      {children}
    </div>
  );
}

export default AppShell;
