/** @jsxImportSource @opentui/react */
import React from 'react';
import type { Message } from '@firmius/shared/api';

export interface ToolCallBlockProps {
  tool: NonNullable<Message['toolCalls']>[number];
}

export const ToolCallBlock: React.FC<ToolCallBlockProps> = ({ tool }) => {
  const isDone = tool.status === 'done';
  const isError = tool.status === 'error';
  const isRunning = tool.status === 'running';

  return (
    <box 
      flexDirection="column" 
      width="100%" 
      marginBottom={1} 
      padding={1} 
      backgroundColor="#111111"
    >
      <box flexDirection="row" justifyContent="space-between">
        <box flexDirection="row">
          <text>
            <color fg="#555555">TOOL </color>
          </text>
          <text>
            <bold>
              <color fg="#00AAFF">{tool.name}</color>
            </bold>
          </text>
        </box>
        <box>
          {isRunning && (
            <text>
              <color fg="#FFFF00">RUNNING</color>
            </text>
          )}
          {isDone && (
            <text>
              <color fg="#00FF00">SUCCESS</color>
            </text>
          )}
          {isError && (
            <text>
              <color fg="#FF0000">ERROR</color>
            </text>
          )}
        </box>
      </box>

      {tool.args !== undefined && (
        <box marginTop={1}>
          <text>
            <color fg="#444444">
              {typeof tool.args === 'string'
                ? tool.args
                : JSON.stringify(tool.args as object)}
            </color>
          </text>
        </box>
      )}

      {tool.result !== undefined && (
        <box marginTop={1} paddingTop={1} borderColor="#222222">
          <text>
            <color fg="#888888">{String(tool.result)}</color>
          </text>
        </box>
      )}
    </box>
  );
};
