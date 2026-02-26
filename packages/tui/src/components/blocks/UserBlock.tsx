/** @jsxImportSource @opentui/react */

interface UserBlockProps {
  content: string;
}

/**
 * UserBlock: Crisp white text, right-aligned or with a distinct background block.
 */
export function UserBlock({ content }: UserBlockProps) {
  // Sanitize content to ensure no leading/trailing newlines that could be interpreted as separate nodes
  const sanitized = content.trim();
  return (
    <box 
      flexDirection="row" 
      justifyContent="flex-end" 
      width="100%" 
      marginBottom={1}
    >
      <box 
        backgroundColor="#FFFFFF" 
        paddingX={1}
      >
        <text fg="#000000">{sanitized}</text>
      </box>
    </box>
  );
}
