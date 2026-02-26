'use client';

import React from 'react';
import { motion } from 'framer-motion';
import { cn } from '@/lib/utils';

interface LoadingScreenProps {
  message?: string;
  fullScreen?: boolean;
}

export function LoadingScreen({ message, fullScreen = false }: LoadingScreenProps) {
  return (
    <motion.div
      initial={{ opacity: 0, scale: 0.92 }}
      animate={{ opacity: 1, scale: 1 }}
      exit={{ opacity: 0, scale: 1.08 }}
      transition={{ duration: 0.45, ease: [0.22, 1, 0.36, 1] }}
      className={cn(
        'flex flex-col items-center justify-center bg-background z-50',
        fullScreen ? 'fixed inset-0 h-screen w-screen' : 'absolute inset-0 h-full w-full'
      )}
    >
      <div className="relative flex flex-col items-center">
        {/* Logo container with glint clipped to logo shape */}
        <div className="loading-logo-wrap">
          {/* 
            The trick: We use the logo as a mask so the glint only shines 
            through the white parts of the logo. The logo itself is rendered
            normally underneath, and the glint layer sits on top with
            mix-blend-mode: screen so it only brightens the logo pixels.
          */}
          <div className="loading-logo-base">
            <img
              src="/logo-both-text-white.webp"
              alt="Firmius"
              className="h-14 sm:h-16 w-auto"
              draggable={false}
            />
          </div>

          {/* Glint layer: masked to the logo shape */}
          <div className="loading-glint-mask">
            <div className="loading-glint-beam" />
          </div>

          {/* Ambient glow behind logo */}
          <div className="loading-ambient-glow" />
        </div>

        {/* Optional message */}
        {message && (
          <motion.p
            initial={{ opacity: 0, y: 8 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ delay: 0.25, duration: 0.4 }}
            className="mt-6 text-xs font-medium text-muted-foreground/60 tracking-wider uppercase"
          >
            {message}
          </motion.p>
        )}
      </div>

      <style dangerouslySetInnerHTML={{
        __html: `
        .loading-logo-wrap {
          position: relative;
          display: flex;
          align-items: center;
          justify-content: center;
          padding: 20px 32px;
        }

        .loading-logo-base {
          position: relative;
          z-index: 1;
        }

        /* Glint mask: uses the same logo image as a CSS mask so the glint 
           beam is clipped to the exact shape of the logo */
        .loading-glint-mask {
          position: absolute;
          inset: 0;
          z-index: 2;
          display: flex;
          align-items: center;
          justify-content: center;
          overflow: hidden;
          /* Use the logo as a mask — glint only visible where logo pixels are */
          -webkit-mask-image: url('/logo-both-text-white.webp');
          mask-image: url('/logo-both-text-white.webp');
          -webkit-mask-size: auto 56px;
          mask-size: auto 56px;
          -webkit-mask-position: center;
          mask-position: center;
          -webkit-mask-repeat: no-repeat;
          mask-repeat: no-repeat;
          pointer-events: none;
        }

        @media (min-width: 640px) {
          .loading-glint-mask {
            -webkit-mask-size: auto 64px;
            mask-size: auto 64px;
          }
        }

        .loading-glint-beam {
          position: absolute;
          top: -20%;
          width: 35%;
          height: 140%;
          background: linear-gradient(
            105deg,
            transparent 0%,
            rgba(255, 255, 255, 0.05) 25%,
            rgba(255, 255, 255, 0.35) 45%,
            rgba(255, 255, 255, 0.6) 50%,
            rgba(255, 255, 255, 0.35) 55%,
            rgba(255, 255, 255, 0.05) 75%,
            transparent 100%
          );
          animation: glint-sweep 2.8s ease-in-out infinite;
          transform: skewX(-15deg);
        }

        .loading-ambient-glow {
          position: absolute;
          top: 50%;
          left: 50%;
          width: 200%;
          height: 200%;
          transform: translate(-50%, -50%);
          background: radial-gradient(
            ellipse at center,
            rgba(255, 255, 255, 0.06) 0%,
            rgba(255, 255, 255, 0.02) 30%,
            transparent 65%
          );
          z-index: 0;
          animation: glow-breathe 3s ease-in-out infinite alternate;
          pointer-events: none;
        }

        @keyframes glint-sweep {
          0%   { left: -50%; opacity: 0; }
          15%  { opacity: 1; }
          85%  { opacity: 1; }
          100% { left: 120%; opacity: 0; }
        }

        @keyframes glow-breathe {
          0%   { opacity: 0.3; transform: translate(-50%, -50%) scale(1); }
          100% { opacity: 0.7; transform: translate(-50%, -50%) scale(1.15); }
        }
      `}} />
    </motion.div>
  );
}

export default LoadingScreen;
