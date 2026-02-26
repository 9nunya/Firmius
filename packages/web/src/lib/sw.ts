'use client';

import { useEffect, useState } from 'react';

// Service Worker registration and cache management
export function useServiceWorker() {
  const [updateAvailable, setUpdateAvailable] = useState(false);

  useEffect(() => {
    if (typeof window === 'undefined' || !('serviceWorker' in navigator)) {
      return;
    }

    const registerSW = async () => {
      try {
        const registration = await navigator.serviceWorker.register('/sw.js');

        // Check for updates
        registration.addEventListener('updatefound', () => {
          const newWorker = registration.installing;
          if (newWorker) {
            newWorker.addEventListener('statechange', () => {
              if (newWorker.state === 'installed' && navigator.serviceWorker.controller) {
                setUpdateAvailable(true);
              }
            });
          }
        });

        // Listen for messages from SW
        navigator.serviceWorker.addEventListener('message', (event) => {
          if (event.data === 'UPDATE_AVAILABLE') {
            setUpdateAvailable(true);
          }
        });
      } catch (error) {
        console.error('SW registration failed:', error);
      }
    };

    registerSW();
  }, []);

  const clearCacheAndReload = async () => {
    if (!('serviceWorker' in navigator)) {
      window.location.reload();
      return;
    }

    // Unregister all service workers
    const registrations = await navigator.serviceWorker.getRegistrations();
    await Promise.all(registrations.map((reg) => reg.unregister()));

    // Clear all caches
    const cacheNames = await caches.keys();
    await Promise.all(cacheNames.map((name) => caches.delete(name)));

    // Clear localStorage app state
    localStorage.removeItem('app-store');

    // Reload
    window.location.reload();
  };

  return { updateAvailable, clearCacheAndReload };
}

// Clear all app data - use this for debugging/stuck state
export async function clearAllAppData() {
  if (typeof window === 'undefined') return;

  // Clear localStorage
  localStorage.clear();
  sessionStorage.clear();

  // Clear IndexedDB
  const databases = await window.indexedDB.databases?.() || [];
  await Promise.all(
    databases.map((db) => {
      if (db.name) {
        return new Promise((resolve) => {
          const req = window.indexedDB.deleteDatabase(db.name!);
          req.onsuccess = resolve;
          req.onerror = resolve;
        });
      }
    })
  );

  // Clear caches
  if ('caches' in window) {
    const cacheNames = await caches.keys();
    await Promise.all(cacheNames.map((name) => caches.delete(name)));
  }

  // Unregister service workers
  if ('serviceWorker' in navigator) {
    const registrations = await navigator.serviceWorker.getRegistrations();
    await Promise.all(registrations.map((reg) => reg.unregister()));
  }
}
