/** @jsxImportSource @opentui/react */
import { useMemo } from 'react';
import useAppStore from '../../store/appStore';
import { StatusGradientBar } from '../core/StatusGradientBar';
import { Composer } from '../core/Composer';
import { useTerminalDimensions } from '@opentui/react';
import { LogStream } from '../LogStream';
import { CommandPalette } from '../modules/CommandPalette';
import { ThreadWizard } from '../modules/ThreadWizard';
import { ThreadsList } from '../modules/ThreadsList';
import { AgentsList } from '../modules/AgentsList';
import { ModelSelector } from '../modules/ModelSelector';
import { FleetModal } from '../modules/FleetModal';

function AgentInspector() {
  const agents = useAppStore((state) => state.agents);
  const activeAgentId = useAppStore((state) => state.activeAgentId);
  const focusAgent = useAppStore((state) => state.focusAgent);

  if (!agents || agents.length === 0) return null;

  return (
    <box height={1} width="100%" flexDirection="row" marginBottom={1}>
      {agents.map((agent, i) => {
        const isActive = agent.id === activeAgentId;
        const label = `${i + 1}:${agent.readableName.toUpperCase()}`;
        return (
          <box 
            key={agent.id} 
            height={1} 
            paddingX={1} 
            backgroundColor={isActive ? '#00FF00' : '#222222'} 
            marginRight={1}
            border={false}
          >
            <text fg={isActive ? '#000000' : '#AAAAAA'} onMouseDown={() => focusAgent(agent.id)}>{label}</text>
          </box>
        );
      })}
    </box>
  );
}

export function MainView() {
  const dimensions = useTerminalDimensions();
  const width = dimensions?.width;
  const height = dimensions?.height;
  
  const activeThreadId = useAppStore((state) => state.activeThreadId);
  const threads = useAppStore((state) => state.threads);
  const activeAgentId = useAppStore((state) => state.activeAgentId);
  const agents = useAppStore((state) => state.agents);
  const isLoading = useAppStore((state) => state.isLoading);
  const connectionStatus = useAppStore((state) => state.connectionStatus);
  
  const activeThread = useMemo(() => threads.find(t => t.id === activeThreadId), [threads, activeThreadId]);

  const breadcrumb = useMemo(() => {
    const activeAgent = agents.find((a) => a.id === activeAgentId);
    return activeAgent ? `AGENTS > ${activeAgent.readableName.toUpperCase()}` : 'AGENTS > NONE';
  }, [agents, activeAgentId]);

  if (!width || !height) return null;

  return (
    <box width="100%" height="100%" flexDirection="column" backgroundColor="#050505" padding={0}>
      <CommandPalette /><ThreadWizard /><ThreadsList /><AgentsList /><ModelSelector /><FleetModal />
      
      {/* HEADER */}
      <box height={5} width="100%" paddingX={1} paddingTop={1} flexDirection="column" flexShrink={0}>
        <AgentInspector />
        <box height={1} flexDirection="row" justifyContent="space-between">
          <text fg="#00FF00"><b>{breadcrumb}</b></text>
          <text fg="#666666">
            <span fg={connectionStatus === 'connected' ? '#00FF00' : '#FF0000'}>● </span>
            {activeThread?.title || "NO THREAD SELECTED"}
          </text>
        </box>
        <box height={1} width="100%" backgroundColor="#1A1A1A" marginTop={1} />
      </box>

      {/* CHAT AREA */}
      <box flexGrow={1} width="100%" padding={0}>
        <LogStream />
      </box>

      {/* FOOTER */}
      <box width="100%" flexDirection="column" flexShrink={0}>
        <box width="100%" height={1} backgroundColor="#1A1A1A" />
        <Composer />
        <StatusGradientBar 
          pulse={isLoading}
          tokensUsed={activeThread?.tokensUsed || 0}
          tokensLimit={activeThread?.tokensLimit || 0}
          modelName={activeThread?.modelId || 'None'}
          providerId={activeThread?.providerId || 'None'}
        />
      </box>
    </box>
  );
}
