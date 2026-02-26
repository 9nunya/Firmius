/** @jsxImportSource @opentui/react */
import { useMemo, useState } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../../store/appStore";

interface Command {
  id: string;
  label: string;
  action: () => void;
  category?: string;
}

export function CommandPalette() {
  const isCommandPaletteOpen = useAppStore((state) => state.isCommandPaletteOpen);
  const setCommandPaletteOpen = useAppStore((state) => state.setCommandPaletteOpen);
  const agents = useAppStore((state) => state.agents);
  const focusAgent = useAppStore((state) => state.focusAgent);
  const setActiveModal = useAppStore((state) => state.setActiveModal);
  const interruptThread = useAppStore((state) => state.interruptThread);

  const [search, setSearch] = useState("");
  const [selectedIndex, setSelectedIndex] = useState(0);

  const commands = useMemo(() => {
    const staticCmds: Command[] = [
      { id: "new-thread", label: "New Thread", category: "System", action: () => { setActiveModal("wizard"); } },
      { id: "interrupt", label: "Interrupt execution", category: "System", action: () => { interruptThread(); } },
      { id: "switch-model", label: "Switch Model", category: "Settings", action: () => { setActiveModal("models"); } },
      {
        id: "toggle-work",
        label: "Toggle Work History",
        category: "Navigation",
        action: () => {
            const { messages, toggleTurnCollapse } = useAppStore.getState();
            // Toggle all turns that have work
            messages.forEach(m => {
                if (m.isUser) {
                    toggleTurnCollapse(`turn-${m.sequence}`);
                }
            });
        },
      },
      { id: "threads-modal", label: "Open Threads List", category: "Navigation", action: () => { setActiveModal("threads"); } },
      { id: "agents-modal", label: "Open Agents List", category: "Navigation", action: () => { setActiveModal("agents"); } },
    ];
    const agentCmds: Command[] = agents.map((agent) => ({
      id: `focus-agent-${agent.id}`,
      label: `Focus Agent: ${agent.readableName}`,
      category: "Agents",
      action: () => focusAgent(agent.id),
    }));
    return [...staticCmds, ...agentCmds];
  }, [agents, interruptThread, focusAgent, setActiveModal]);

  const filteredCommands = useMemo(() => {
    if (!search) return commands;
    const s = search.toLowerCase();
    return commands
      .filter((cmd) => cmd.label.toLowerCase().includes(s) || cmd.category?.toLowerCase().includes(s))
      .sort((a, b) => {
        const aStarts = a.label.toLowerCase().startsWith(s);
        const bStarts = b.label.toLowerCase().startsWith(s);
        if (aStarts && !bStarts) return -1;
        if (!aStarts && bStarts) return 1;
        return 0;
      });
  }, [commands, search]);

  useKeyboard((key) => {
    if (key.ctrl && key.name === "p") {
      setCommandPaletteOpen(!isCommandPaletteOpen);
      setSearch("");
      setSelectedIndex(0);
      return;
    }
    if (!isCommandPaletteOpen) return;
    if (key.name === "escape") {
      setCommandPaletteOpen(false);
      return;
    }
    if (key.name === "up") {
      setSelectedIndex((prev) => (prev > 0 ? prev - 1 : Math.max(0, filteredCommands.length - 1)));
    } else if (key.name === "down") {
      setSelectedIndex((prev) => (prev < filteredCommands.length - 1 ? prev + 1 : 0));
    } else if (key.name === "enter" || key.name === "return") {
      const selected = filteredCommands[selectedIndex];
      if (selected) {
        setCommandPaletteOpen(false);
        setTimeout(() => selected.action(), 0);
      }
    } else if (key.name === "backspace" || key.name === "delete") {
      setSearch((prev) => prev.slice(0, -1));
      setSelectedIndex(0);
    } else if (key.name.length === 1 && !key.ctrl && !key.meta) {
      const char = key.shift ? key.name.toUpperCase() : key.name;
      setSearch((prev) => prev + char);
      setSelectedIndex(0);
    }
  });

  if (!isCommandPaletteOpen) return null;

  const paletteHeight = Math.min(15, filteredCommands.length + 4);

  return (
    <box
      position="absolute"
      top={2}
      left="50%"
      marginLeft={-40}
      width={80}
      height={paletteHeight}
      flexDirection="column"
      zIndex={10}
      backgroundColor="#0A0A0A"
    >
      <box paddingX={1} paddingY={1} backgroundColor="#1A1A1A" flexDirection="row">
        <text><span fg="#00FF00"><b>{"> "}</b></span>{search}<span bg="#00FF00" fg="#000000"> </span></text>
      </box>
      <box flexGrow={1} flexDirection="column">
        {filteredCommands.length === 0 ? (
          <box padding={1}>
            <text fg="#666666">No commands found</text>
          </box>
        ) : (
          filteredCommands.slice(0, 10).map((cmd, idx) => {
            const isActive = idx === selectedIndex;
            return (
              <box
                key={cmd.id}
                paddingX={1}
                backgroundColor={isActive ? "#333333" : "transparent"}
                flexDirection="row"
              >
                <text fg={isActive ? "#00FF00" : "#EEEEEE"}>
                  {`${isActive ? "► " : "  "}[${cmd.category || "General"}] ${cmd.label}`}
                </text>
              </box>
            );
          })
        )}
      </box>
    </box>
  );
}
