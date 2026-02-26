import type { Metadata } from 'next';
import './globals.css';
import { ThemeProvider } from '@/components/layout/ThemeProvider';
import { UpdateBanner } from '@/components/layout/UpdateBanner';

export const metadata: Metadata = {
  title: 'Firmius',
  description: 'AI-powered development assistant',
  icons: {
    icon: '/logo-white.webp',
    shortcut: '/logo-white.webp',
    apple: '/logo-white.webp',
  },
  manifest: '/manifest.json',
  other: {
    'apple-mobile-web-app-capable': 'yes',
    'apple-mobile-web-app-status-bar-style': 'black-translucent',
    'apple-mobile-web-app-title': 'Firmius',
  },
};

export const viewport = {
  width: 'device-width' as const,
  initialScale: 1,
  maximumScale: 1,
  userScalable: false,
  viewportFit: 'cover' as const,
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en" suppressHydrationWarning>
      <body>
        <ThemeProvider
          attribute="class"
          defaultTheme="dark"
          enableSystem={false}
          disableTransitionOnChange
        >
          {children}
          <UpdateBanner />
        </ThemeProvider>
      </body>
    </html>
  );
}
