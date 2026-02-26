/** @jsxImportSource @opentui/react */
import { useState, useEffect } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../../store/appStore";

/**
 * AgentsList: Modal for listing and selecting agents within the current thread.
 */
export function AgentsList() {
  const activeModal = useAppStore((state) => state.activeModal);
  const setActiveModal = useAppStore((state) => state.setActiveModal);
  const agents = useAppStore((state) => state.agents);
  const focusAgent = useAppStore((state) => state.focusAgent);

  const [selectedIndex, setSelectedIndex] = useState(0);

  useEffect(() => {
    if (activeModal === "agents") {
      setSelectedIndex(0);
    }
  }, [activeModal]);

  useKeyboard((key) => {
    if (activeModal !== "agents") return;
    if (key.name === "escape") {
      setActiveModal(null);
      return;
    }
    if (key.name === "up") {
      setSelectedIndex((prev) => (prev > 0 ? prev - 1 : Math.max(0, agents.length - 1)));
    } else if (key.name === "down") {
      setSelectedIndex((prev) => (prev < agents.length - 1 ? prev + 1 : 0));
    } else if (key.name === "enter" || key.name === "return") {
      const selected = agents[selectedIndex];
      if (selected) {
        focusAgent(selected.id);
        setActiveModal(null);
      }
    }
  });

  if (activeModal !== "agents") return null;

  const modalWidth = 60;
  const modalHeight = 15;

  return (
    <box
      position="absolute"
      top="50%"
      left="50%"
      marginTop={-7}
      marginLeft={-30}
      width={modalWidth}
      height={modalHeight}
      zIndex={20}
      backgroundColor="#0A0A0A"
    >
      <box paddingX={1} paddingY={1} backgroundColor="#1A1A1A" flexDirection="row" justifyContent="space-between">
        <text fg="#00FF00"><b>AGENT LIST</b></text>
        <text fg="#444444">ESC TO EXIT // ENTER TO FOCUS</text>
      </box>

      <box flexGrow={1} padding={1} flexDirection="column">
        {agents.length === 0 ? (
          <box flexGrow={1} justifyContent="center" alignItems="center">
            <text fg="#666666">No agents active in this thread.</text>
          </box>
        ) : (
          agents.map((agent, idx) => {
            const isActive = idx === selectedIndex;
            return (
              <box
                key={agent.id}
                paddingX={1}
                backgroundColor={isActive ? "#333333" : "transparent"}
                flexDirection="row"
              >
                <text fg={isActive ? "#00FF00" : "#EEEEEE"}>
                  {`${isActive ? "► " : "  "}${agent.readableName.toUpperCase()}`}
                </text>
                <text fg="#666666">{` (${agent.purpose})`}</text>
              </box>
            );
          })
        )}
      </box>
    </box>
  );
}
