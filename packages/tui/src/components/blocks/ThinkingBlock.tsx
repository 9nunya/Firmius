/** @jsxImportSource @opentui/react */

export interface ThinkingBlockProps {
  content: string;
  isStreaming?: boolean;
}

export function ThinkingBlock({ content, isStreaming }: ThinkingBlockProps) {
  const textColor = '#BBBBBB';
  const dotColor = '#00FF00';
  return (
    <box flexDirection="row" width="100%" marginBottom={1} paddingLeft={1}>
      <box width={1} backgroundColor="#222222" marginRight={1} />
      <box flexDirection="column" flexGrow={1}>
        <box flexDirection="row" marginBottom={1}>
          <text fg={textColor}><i>THINKING</i></text>
          {isStreaming && <text fg={dotColor}><b> ◒</b></text>}
        </box>
        <text fg="#666666">{content}</text>
      </box>
    </box>
  );
}
