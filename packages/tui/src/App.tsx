/** @jsxImportSource @opentui/react */
import { useEffect } from 'react';
import { MainView } from './components/layout/MainView';
import useAppStore from './store/appStore';

/**
 * App Root: Bootstraps the Firmius TUI.
 * Wraps the application in necessary providers and initializes the MainView.
 */
export function App() {
  const loadThreads = useAppStore(state => state.loadThreads);
  const loadProviders = useAppStore(state => state.loadProviders);

  useEffect(() => {
    loadThreads();
    loadProviders();
  }, [loadThreads, loadProviders]);

  return (
    <box width="100%" height="100%">
      <MainView />
    </box>
  );
}
