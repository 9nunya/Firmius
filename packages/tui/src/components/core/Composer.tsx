/** @jsxImportSource @opentui/react */
import { useTerminalInput } from "../../hooks/useTerminalInput";

/**
 * Composer component: A borderless textarea-like input for the TUI.
 */
export function Composer() {
  const { inputBuffer, cursorPosition } = useTerminalInput();

  const cursorBg = "#00FF00";
  const cursorFg = "#000000";

  if (inputBuffer.length === 0) {
    return (
      <box width="100%" height={1} paddingX={1} backgroundColor="#0F0F0F" flexDirection="row">
        <text><span bg={cursorBg} fg={cursorFg}> </span> Type a message...</text>
      </box>
    );
  }

  const before = inputBuffer.slice(0, cursorPosition);
  const charAtCursor = cursorPosition < inputBuffer.length ? inputBuffer[cursorPosition] : " ";
  const after = inputBuffer.slice(cursorPosition + 1);

  return (
    <box width="100%" height={1} paddingX={1} backgroundColor="#0F0F0F" flexDirection="row">
      <text>{before}<span bg={cursorBg} fg={cursorFg}>{charAtCursor}</span>{after}</text>
    </box>
  );
}
