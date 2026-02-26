/** @jsxImportSource @opentui/react */
import { useState, useEffect, useMemo } from "react";
import { useKeyboard } from "@opentui/react";
import useAppStore from "../../store/appStore";

type AgentStatus = 'initializing' | 'working' | 'idle' | 'error';

function getStatusColor(status: AgentStatus): string {
  switch (status) {
    case 'working': return '#0088FF';
    case 'idle': return '#00FF00';
    case 'error': return '#FF0000';
    case 'initializing':
    default: return '#666666';
  }
}

function getStatusIcon(status: AgentStatus): string {
  switch (status) {
    case 'working': return '●';
    case 'idle': return '✓';
    case 'error': return '✗';
    case 'initializing':
    default: return '○';
  }
}

export function FleetModal() {
  const activeModal = useAppStore((state) => state.activeModal);
  const setActiveModal = useAppStore((state) => state.setActiveModal);
  const agents = useAppStore((state) => state.agents);
  const activeAgentId = useAppStore((state) => state.activeAgentId);
  const focusAgent = useAppStore((state) => state.focusAgent);
  const activeThreadId = useAppStore((state) => state.activeThreadId);

  const [selectedIndex, setSelectedIndex] = useState(0);
  const [expandedAgents, setExpandedAgents] = useState<Set<string>>(new Set());

  const threadAgents = useMemo(() => {
    return agents.filter(a => a.threadId === activeThreadId || !a.threadId);
  }, [agents, activeThreadId]);

  const agentTree = useMemo(() => {
    const tree: Map<string | null, typeof agents> = new Map();
    for (const agent of threadAgents) {
      const parentKey = agent.parentId ?? (agent.isLead ? null : null);
      if (!tree.has(parentKey)) {
        tree.set(parentKey, []);
      }
      tree.get(parentKey)!.push(agent);
    }
    return tree;
  }, [threadAgents]);

  const rootAgents = useMemo(() => {
    return agentTree.get(null) ?? threadAgents.filter(a => a.isLead || !a.parentId);
  }, [agentTree, threadAgents]);

  const flatAgents = useMemo(() => {
    const result: Array<{ agent: typeof agents[0]; depth: number }> = [];
    const traverse = (agentList: typeof agents, depth: number) => {
      for (const agent of agentList) {
        result.push({ agent, depth });
        const children = agentTree.get(agent.id) ?? [];
        if (children.length > 0 && expandedAgents.has(agent.id)) {
          traverse(children, depth + 1);
        }
      }
    };
    traverse(rootAgents, 0);
    return result;
  }, [rootAgents, agentTree, expandedAgents]);

  useEffect(() => {
    if (activeModal === "fleet") {
      setSelectedIndex(0);
    }
  }, [activeModal]);

  useKeyboard((key) => {
    if (activeModal !== "fleet") return;
    if (key.name === "escape") {
      setActiveModal(null);
      return;
    }
    if (key.name === "up") {
      setSelectedIndex((prev) => (prev > 0 ? prev - 1 : flatAgents.length - 1));
    } else if (key.name === "down") {
      setSelectedIndex((prev) => (prev < flatAgents.length - 1 ? prev + 1 : 0));
    } else if (key.name === "enter" || key.name === "return") {
      const selected = flatAgents[selectedIndex];
      if (selected) {
        focusAgent(selected.agent.id);
        setActiveModal(null);
      }
    } else if (key.name === "left" || key.name === "right") {
      const selected = flatAgents[selectedIndex];
      if (selected) {
        const children = agentTree.get(selected.agent.id) ?? [];
        if (children.length > 0) {
          setExpandedAgents(prev => {
            const next = new Set(prev);
            if (next.has(selected.agent.id)) {
              next.delete(selected.agent.id);
            } else {
              next.add(selected.agent.id);
            }
            return next;
          });
        }
      }
    }
  });

  if (activeModal !== "fleet") return null;

  const modalWidth = 70;
  const modalHeight = 20;

  const workingCount = threadAgents.filter(a => a.status === 'working').length;
  const errorCount = threadAgents.filter(a => a.status === 'error').length;

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
    >
      <box paddingX={1} paddingY={1} backgroundColor="#1A1A1A" flexDirection="row" justifyContent="space-between">
        <text fg="#00FF00"><b>FLEET STATUS</b></text>
        <box flexDirection="row">
          {workingCount > 0 && <text fg="#0088FF"> {workingCount} ACTIVE </text>}
          {errorCount > 0 && <text fg="#FF0000"> {errorCount} ERROR </text>}
          <text fg="#444444"> ESC TO CLOSE // ENTER TO FOCUS</text>
        </box>
      </box>

      <box flexGrow={1} padding={1} flexDirection="column">
        {flatAgents.length === 0 ? (
          <box flexGrow={1} justifyContent="center" alignItems="center">
            <text fg="#666666">No agents spawned in this thread.</text>
          </box>
        ) : (
          flatAgents.map(({ agent, depth }, idx) => {
            const isActive = idx === selectedIndex;
            const isFocused = agent.id === activeAgentId;
            const children = agentTree.get(agent.id) ?? [];
            const hasChildren = children.length > 0;
            const isExpanded = expandedAgents.has(agent.id);
            const indent = "  ".repeat(depth);
            const expandIcon = hasChildren ? (isExpanded ? "▼" : "▶") : " ";
            const statusIcon = getStatusIcon(agent.status as AgentStatus);
            const statusColor = getStatusColor(agent.status as AgentStatus);

            return (
              <box
                key={agent.id}
                paddingX={1}
                backgroundColor={isActive ? "#333333" : "transparent"}
                flexDirection="row"
              >
                <text fg={isActive ? "#00FF00" : "#EEEEEE"}>
                  {`${isActive ? "► " : "  "}${indent}${expandIcon} ${statusIcon} ${agent.readableName.toUpperCase()}`}
                </text>
                <text fg={statusColor}>{` (${agent.purpose})`}</text>
                {isFocused && <text fg="#FFFF00"> [FOCUSED]</text>}
                {agent.isLead && <text fg="#00FF00"> [LEAD]</text>}
              </box>
            );
          })
        )}
      </box>

      <box paddingX={1} backgroundColor="#1A1A1A" flexDirection="row" justifyContent="space-between">
        <text fg="#666666">◄/►: Expand/Collapse</text>
        <text fg="#666666">Total: {threadAgents.length} agents</text>
      </box>
    </box>
  );
}
