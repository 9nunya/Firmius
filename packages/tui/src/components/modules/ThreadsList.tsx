/** @jsxImportSource @opentui/react */
import { useState, useEffect } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../../store/appStore";

/**
 * ThreadsList: Modal for listing and selecting active threads.
 */
export function ThreadsList() {
  const activeModal = useAppStore((state) => state.activeModal);
  const setActiveModal = useAppStore((state) => state.setActiveModal);
  const threads = useAppStore((state) => state.threads);
  const selectThread = useAppStore((state) => state.selectThread);
  const [selectedIndex, setSelectedIndex] = useState(0);

  useEffect(() => {
    if (activeModal === "threads") {
      setSelectedIndex(0);
    }
  }, [activeModal]);

  useKeyboard((key) => {
    if (activeModal !== "threads") return;
    if (key.name === "escape") {
      setActiveModal(null);
      return;
    }
    if (key.name === "up") {
      setSelectedIndex((prev) => (prev > 0 ? prev - 1 : Math.max(0, threads.length - 1)));
    } else if (key.name === "down") {
      setSelectedIndex((prev) => (prev < threads.length - 1 ? prev + 1 : 0));
    } else if (key.name === "enter" || key.name === "return") {
      const selected = threads[selectedIndex];
      if (selected) {
        selectThread(selected.id);
        setActiveModal(null);
      }
    }
  });

  if (activeModal !== "threads") return null;

  const modalWidth = 70;
  const modalHeight = 20;

  return (
    <box
      position="absolute"
      top="50%"
      left="50%"
      marginTop={-10}
      marginLeft={-35}
      width={modalWidth}
      height={modalHeight}
      zIndex={20}
      backgroundColor="#0A0A0A"
      padding={0}
    >
      <box paddingX={1} paddingY={1} backgroundColor="#1A1A1A" flexDirection="row" justifyContent="space-between">
        <text fg="#00FF00"><b>THREAD LIST</b></text>
        <text fg="#444444">ESC TO EXIT // ENTER TO SELECT</text>
      </box>
      <box flexGrow={1} padding={1} flexDirection="column">
        {threads.length === 0 ? (
          <box flexGrow={1} justifyContent="center" alignItems="center">
            <text fg="#666666">No threads found. Create one with Ctrl+P.</text>
          </box>
        ) : (
          threads.slice(0, 15).map((thread, idx) => {
            const isActive = idx === selectedIndex;
            const timeStr = thread.checkpointedAt ? new Date(thread.checkpointedAt).toLocaleTimeString() : "N/A";
            const title = thread.title || "Untitled Thread";
            const prefix = isActive ? "► " : "  ";
            
            return (
              <box
                key={thread.id}
                paddingX={1}
                backgroundColor={isActive ? "#333333" : "transparent"}
                flexDirection="row"
                justifyContent="space-between"
              >
                <text fg={isActive ? "#00FF00" : "#EEEEEE"}>{prefix + title}</text>
                <text fg="#666666">{timeStr}</text>
              </box>
            );
          })
        )}
      </box>
    </box>
  );
}
