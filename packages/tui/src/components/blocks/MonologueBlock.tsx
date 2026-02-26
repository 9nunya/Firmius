/** @jsxImportSource @opentui/react */

interface MonologueBlockProps {
  content: string;
}

/**
 * MonologueBlock: Indented 2 spaces, using a color-coded vertical bar 
 * to indicate "Internal Thought".
 */
export function MonologueBlock({ content }: MonologueBlockProps) {
  const sanitized = content.trim();
  return (
    <box 
      flexDirection="row" 
      paddingLeft={2} 
      marginBottom={1}
    >
      <box 
        width={1} 
        backgroundColor="#333333" 
        marginRight={1}
      />
      <box flexGrow={1}>
        <text fg="#AAAAAA">{sanitized}</text>
      </box>
    </box>
  );
}
