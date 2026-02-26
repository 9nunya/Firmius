import React, { useState } from 'react';

interface ReasoningBlockProps {
  content: string;
}

/**
 * ReasoningBlock: Dimmed, italicized text. Sequential.
 * If reasoning is long, it should be collapsible or shown with a "Reasoning" header 
 * on a slightly different charcoal background (#1A1A1A).
 */
export const ReasoningBlock: React.FC<ReasoningBlockProps> = ({ content }) => {
  const [isCollapsed, setIsCollapsed] = useState(false);
  const isLong = content.length > 200;

  return (
    <box 
      flexDirection="column" 
      marginBottom={1}
      backgroundColor="#121212"
    >
      <box 
        flexDirection="row" 
        justifyContent="space-between" 
        paddingX={1}
        backgroundColor="#1A1A1A"
      >
        <text fg="#666666">
          <em>REASONING</em>
        </text>
        {isLong && (
          <text fg="#444444" onMouseDown={() => setIsCollapsed(!isCollapsed)}>
            {isCollapsed ? "[+]" : "[-]"}
          </text>
        )}
      </box>
      
      {!isCollapsed && (
        <box padding={1}>
          <text fg="#888888">
            <em>{content}</em>
          </text>
        </box>
      )}
    </box>
  );
};
