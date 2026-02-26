import { useCallback, useState, useRef } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../store/appStore";
import { GLOBAL_KEYMAP } from "../utils/keymap";

/**
 * Advanced Input Engine Hook for OpenTUI
 * Handles keyboard events, cursor movement, word jumping, and history.
 */
export const useTerminalInput = () => {
  const inputBuffer = useAppStore((state) => state.inputBuffer);
  const cursorPosition = useAppStore((state) => state.cursorPosition);
  const navigationHistory = useAppStore((state) => state.navigationHistory);
  const jumpMode = useAppStore((state) => state.jumpMode);
  const jumpCodes = useAppStore((state) => state.jumpCodes);
   const activeAgentId = useAppStore((state) => state.activeAgentId);
   const agents = useAppStore((state) => state.agents);
   const setActiveModal = useAppStore((state) => state.setActiveModal);
   
   const setInputBuffer = useAppStore((state) => state.setInputBuffer);
  const setCursorPosition = useAppStore((state) => state.setCursorPosition);
  const sendMessage = useAppStore((state) => state.sendMessage);
  const addToNavigationHistory = useAppStore((state) => state.addToNavigationHistory);
  const setJumpMode = useAppStore((state) => state.setJumpMode);
  const focusAgent = useAppStore((state) => state.focusAgent);

  const isCommandPaletteOpen = useAppStore((state) => state.isCommandPaletteOpen);
  const activeModal = useAppStore((state) => state.activeModal);

  const [historyIndex, setHistoryIndex] = useState<number>(-1);
  const [undoStack, setUndoStack] = useState<string[]>([]);
  const jumpInputRef = useRef<string>("");

  const pushUndo = useCallback((content: string) => {
    setUndoStack((prev) => {
      if (prev[0] === content) return prev;
      return [content, ...prev].slice(0, 50);
    });
  }, []);

  const findWordBoundary = useCallback(
    (pos: number, direction: "left" | "right") => {
      if (direction === "left") {
        let i = pos - 1;
        while (i > 0 && inputBuffer[i] === " ") i--;
        while (i > 0 && inputBuffer[i] !== " ") i--;
        return i < 0 ? 0 : i === 0 && inputBuffer[0] !== " " ? 0 : i + 1;
      } else {
        let i = pos;
        while (i < inputBuffer.length && inputBuffer[i] === " ") i++;
        while (i < inputBuffer.length && inputBuffer[i] !== " ") i++;
        return i;
      }
    },
    [inputBuffer]
  );

  useKeyboard((key) => {
    // If command palette or modal is open, we do NOT handle keys here.
    if (isCommandPaletteOpen || activeModal) return;

    // 1. Handle Jump Mode
    if (jumpMode) {
      if (key.name === "escape") {
        setJumpMode(false);
        jumpInputRef.current = "";
        return;
      }
      if (key.name.length === 1) {
        jumpInputRef.current += key.name.toLowerCase();
        if (jumpInputRef.current.length === 2) {
          const sequence = jumpCodes[jumpInputRef.current];
          if (sequence) {
            // Logic to scroll/focus will be handled by store or side effect
            // For now, we just exit jump mode
            // We could add a 'focusedMessageSequence' to store
            setJumpMode(false);
          }
          jumpInputRef.current = "";
        }
      }
      return;
    }

    // 2. Handle Global Keymap
    const globalAction = GLOBAL_KEYMAP.find(
      (m) =>
        m.key === key.name &&
        !!m.ctrl === !!key.ctrl &&
        !!m.meta === !!key.meta &&
        !!m.shift === !!key.shift
    );

     if (globalAction) {
       switch (globalAction.action) {
         case "TOGGLE_JUMP_MODE":
           setJumpMode(true);
           return;
         case "OPEN_FLEET_MODAL":
           setActiveModal('fleet');
           return;
         case "NEXT_AGENT": {
           const currentIndex = agents.findIndex((a) => a.id === activeAgentId);
           const nextIndex = (currentIndex + 1) % agents.length;
           focusAgent(agents[nextIndex]?.id || null);
           return;
         }
         case "CANCEL":
           // already handled escape for jumpMode, but for general cancel:
           return;
       }

      if (globalAction.action.startsWith("SWITCH_AGENT_")) {
        const num = parseInt(globalAction.action.split("_")[2] || "1");
        if (agents[num - 1]) {
          focusAgent(agents[num - 1]!.id);
        }
        return;
      }
    }

    const insertChar = (char: string) => {
      const currentBuffer = useAppStore.getState().inputBuffer;
      const currentPos = useAppStore.getState().cursorPosition;
      pushUndo(currentBuffer);
      const nextContent =
        currentBuffer.slice(0, currentPos) +
        char +
        currentBuffer.slice(currentPos);
      setInputBuffer(nextContent);
      setCursorPosition(currentPos + 1);
    };

    const deleteChar = () => {
      const currentBuffer = useAppStore.getState().inputBuffer;
      const currentPos = useAppStore.getState().cursorPosition;
      if (currentPos > 0) {
        pushUndo(currentBuffer);
        const nextContent =
          currentBuffer.slice(0, currentPos - 1) +
          currentBuffer.slice(currentPos);
        setInputBuffer(nextContent);
        setCursorPosition(currentPos - 1);
      }
    };

    const deleteForward = () => {
      const currentBuffer = useAppStore.getState().inputBuffer;
      const currentPos = useAppStore.getState().cursorPosition;
      if (currentPos < currentBuffer.length) {
        pushUndo(currentBuffer);
        const nextContent =
          currentBuffer.slice(0, currentPos) +
          currentBuffer.slice(currentPos + 1);
        setInputBuffer(nextContent);
      }
    };

    // Handle Key Combos
    if (key.ctrl) {
      switch (key.name) {
        case "a": // Home
          setCursorPosition(0);
          break;
        case "e": // End
          setCursorPosition(inputBuffer.length);
          break;
        case "u": // Clear before cursor
          pushUndo(inputBuffer);
          setInputBuffer(inputBuffer.slice(cursorPosition));
          setCursorPosition(0);
          break;
        case "k": // Clear after cursor
          pushUndo(inputBuffer);
          setInputBuffer(inputBuffer.slice(0, cursorPosition));
          break;
        case "w": // Delete word before cursor
        case "backspace": {
          pushUndo(inputBuffer);
          const start = findWordBoundary(cursorPosition, "left");
          const nextContent =
            inputBuffer.slice(0, start) + inputBuffer.slice(cursorPosition);
          setInputBuffer(nextContent);
          setCursorPosition(start);
          break;
        }
        case "z": // Undo
          if (undoStack.length > 0) {
            const prev = undoStack[0];
            if (prev !== undefined) {
              setUndoStack((s) => s.slice(1));
              setInputBuffer(prev);
              setCursorPosition(Math.min(cursorPosition, prev.length));
            }
          }
          break;
        case "left":
          setCursorPosition(findWordBoundary(cursorPosition, "left"));
          break;
        case "right":
          setCursorPosition(findWordBoundary(cursorPosition, "right"));
          break;
      }
    } else if (key.meta) { // meta = Alt in OpenTUI
      switch (key.name) {
        case "b": // Word left
          setCursorPosition(findWordBoundary(cursorPosition, "left"));
          break;
        case "f": // Word right
          setCursorPosition(findWordBoundary(cursorPosition, "right"));
          break;
      }
    } else {
      switch (key.name) {
        case "left":
          setCursorPosition(Math.max(0, cursorPosition - 1));
          break;
        case "right":
          setCursorPosition(Math.min(inputBuffer.length, cursorPosition + 1));
          break;
        case "up":
          if (navigationHistory.length > 0) {
            const newIndex = Math.min(
              historyIndex + 1,
              navigationHistory.length - 1
            );
            const historyItem = navigationHistory[newIndex];
            if (historyItem !== undefined) {
              setHistoryIndex(newIndex);
              setInputBuffer(historyItem);
              setCursorPosition(historyItem.length);
            }
          }
          break;
        case "down":
          if (historyIndex > 0) {
            const newIndex = historyIndex - 1;
            const historyItem = navigationHistory[newIndex];
            if (historyItem !== undefined) {
              setHistoryIndex(newIndex);
              setInputBuffer(historyItem);
              setCursorPosition(historyItem.length);
            }
          } else if (historyIndex === 0) {
            setHistoryIndex(-1);
            setInputBuffer("");
            setCursorPosition(0);
          }
          break;
        case "home":
          setCursorPosition(0);
          break;
        case "end":
          setCursorPosition(inputBuffer.length);
          break;
        case "backspace":
          deleteChar();
          break;
        case "delete":
          deleteForward();
          break;
        case "enter":
        case "return":
          if (inputBuffer.trim()) {
            sendMessage(inputBuffer);
            addToNavigationHistory(inputBuffer);
            setInputBuffer("");
            setCursorPosition(0);
            setHistoryIndex(-1);
            setUndoStack([]);
          }
          break;
        case "space":
          insertChar(" ");
          break;
        default:
          if (key.name && key.name.length === 1) {
            const char = key.shift ? key.name.toUpperCase() : key.name;
            insertChar(char);
          } else if (!key.name && (key as { sequence?: string }).sequence) {
            insertChar((key as { sequence?: string }).sequence as string);
          }
          break;
      }
    }
  });

  return { inputBuffer, cursorPosition };
};
