/** @jsxImportSource @opentui/react */
import { useMemo } from 'react';
import type { Message } from '@firmius/shared/api';
import useAppStore from '../../store/appStore';
import { UserBlock } from './UserBlock';
import { ThinkingBlock } from './ThinkingBlock';
import { MonologueBlock } from './MonologueBlock';
import { ToolBlock } from './ToolBlock';
import { ResponseBlock } from './ResponseBlock';

export function BlockFactory({ message, jumpCode }: { message: Message; jumpCode?: string }) {
  const jumpMode = useAppStore((state) => state.jumpMode);
  const parts: any[] = [];
  const overlay = useMemo(() => {
    if (!jumpMode || !jumpCode) return null;
    return (
      <box position="absolute" top={0} left={0} width={3} height={1} backgroundColor="#00FF00" paddingX={0.5}>
        <text fg="#000000"><b>{jumpCode.toUpperCase()}</b></text>
      </box>
    );
  }, [jumpMode, jumpCode]);
  if (message.isUser) {
    const userContent = typeof message.content === 'string' ? message.content : JSON.stringify(message.content);
    return (
      <box width="100%" position="relative">
        {overlay}<UserBlock content={userContent} />
      </box>
    );
  }
  if (message.thinking) {
    parts.push(<ThinkingBlock key="thinking" content={message.thinking} isStreaming={message.isStreaming} />);
  }
  if (message.content && (typeof message.content === 'string' ? message.content.length > 0 : true)) {
    const contentStr = typeof message.content === 'string' ? message.content : JSON.stringify(message.content);
    if (message.isMonologue) {
        parts.push(<MonologueBlock key="monologue" content={contentStr} />);
    } else {
        parts.push(<ResponseBlock key="response" content={contentStr} isStreaming={message.isStreaming} />);
    }
  }
  if (message.toolCalls && message.toolCalls.length > 0) {
    message.toolCalls.forEach((tool, idx) => {
      const status: 'running' | 'success' | 'error' = tool.status === 'done' 
        ? 'success' 
        : tool.status === 'error' 
          ? 'error' 
          : 'running';
      const outputStr = tool.result !== undefined ? String(tool.result) : undefined;
      parts.push(
        <ToolBlock 
          key={`tool-${tool.callId || idx}`} 
          toolName={tool.name} 
          state={status} 
          output={outputStr}
        />
      );
    });
  }
  return (
    <box flexDirection="column" width="100%" position="relative">
      {overlay}{parts}
    </box>
  );
}
