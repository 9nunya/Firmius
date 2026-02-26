/** @jsxImportSource @opentui/react */
import { useState, useEffect } from 'react';

interface ToolBlockProps {
  toolName: string;
  state: 'running' | 'success' | 'error' | 'done';
  output?: string;
  error?: string;
}

export function ToolBlock({ toolName, state, output, error }: ToolBlockProps) {
  const [frame, setFrame] = useState(0);
  const frames = ['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧', '⠇', '⠏'];
  useEffect(() => {
    if (state === 'running') {
      const interval = setInterval(() => {
        setFrame((f) => (f + 1) % frames.length);
      }, 80);
      return () => clearInterval(interval);
    }
  }, [state]);
  const renderContent = () => {
    const displayState = state === 'done' ? 'success' : state;
    switch (displayState) {
      case 'running':
        return (
          <box flexDirection="row">
            <text fg="#FFFF00">{frames[frame]}</text><text fg="#FFFFFF">{" Executing " + toolName + "..."}</text>
          </box>
        );
      case 'success':
        return (
          <box flexDirection="column">
            <box flexDirection="row">
              <text fg="#00FF00">{"✔ "}</text><text fg="#FFFFFF">{toolName + " completed"}</text>
            </box>
            {output && (
              <box paddingLeft={2} marginTop={1}>
                <text fg="#888888">{output.slice(0, 100) + (output.length > 100 ? '...' : '')}</text>
              </box>
            )}
          </box>
        );
      case 'error':
        return (
          <box flexDirection="column">
            <box flexDirection="row">
              <text fg="#FF0000">{"✘ "}</text><text fg="#FF0000">{toolName + " failed"}</text>
            </box>
            {error && (
              <box paddingLeft={2} marginTop={1} backgroundColor="#2A0000">
                <text fg="#FF5555">{error}</text>
              </box>
            )}
          </box>
        );
      default:
        return null;
    }
  };
  return (
    <box flexDirection="column" padding={1} marginBottom={1} backgroundColor="#0A0A0A">
      {renderContent()}
    </box>
  );
}
