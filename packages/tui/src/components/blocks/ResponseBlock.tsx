/** @jsxImportSource @opentui/react */
import { MarkdownRenderer } from '../../utils/markdown';

interface ResponseBlockProps {
  content: string;
  isStreaming?: boolean;
}

export function ResponseBlock({ content }: ResponseBlockProps) {
  return (
    <box 
      flexDirection="column" 
      padding={1} 
      marginBottom={1}
    >
      <MarkdownRenderer content={content} />
    </box>
  );
}
