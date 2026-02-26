'use client';

import { useEffect, useCallback, useMemo } from 'react';
import useAppStore from '../stores/app-store';
import { client } from '@firmius/shared/api';
import type { Agent } from '@firmius/shared/api';

interface AgentTree extends Agent {
  children: AgentTree[];
}

export function useAgents() {
  const { activeThreadId, agents, focusAgent, activeAgentId } = useAppStore();

  const loadAgents = useCallback(async (threadId: string) => {
    try {
      const loadedAgents = await client.getAgents(threadId);
      return loadedAgents;
    } catch (error) {
      console.error('Failed to load agents:', error);
      return [];
    }
  }, []);

  const getActiveAgent = useCallback((): Agent | null => {
    if (!activeAgentId) return null;
    return agents.find((a: Agent) => a.id === activeAgentId) || null;
  }, [agents, activeAgentId]);

  const getAgentContextUsage = useCallback((agentId: string): number => {
    return 0;
  }, []);

  const buildAgentTree = useCallback((): AgentTree[] => {
    const agentMap = new Map<string, AgentTree>();
    const rootAgents: AgentTree[] = [];

    agents.forEach((agent) => {
      agentMap.set(agent.id, { ...agent, children: [] });
    });

    agents.forEach((agent) => {
      const agentNode = agentMap.get(agent.id);
      if (!agentNode) return;

      if (agent.parentId) {
        const parent = agentMap.get(agent.parentId);
        if (parent) {
          parent.children.push(agentNode);
        } else {
          rootAgents.push(agentNode);
        }
      } else {
        rootAgents.push(agentNode);
      }
    });

    return rootAgents;
  }, [agents]);

  const getSubagentIds = useCallback((agentId: string): string[] => {
    const agent = agents.find((a: Agent) => a.id === agentId);
    if (!agent || !agent.subagentIds) return [];

    const result: string[] = [...agent.subagentIds];
    agent.subagentIds.forEach((subId) => {
      // Basic check to prevent some obvious circular recursion, though not full cycle detection
      if (subId !== agentId) {
        result.push(...getSubagentIds(subId));
      }
    });

    return result;
  }, [agents]);

  const agentTree = useMemo(() => buildAgentTree(), [buildAgentTree]);
  const activeAgent = useMemo(() => getActiveAgent(), [getActiveAgent]);

  useEffect(() => {
    if (activeThreadId) {
      loadAgents(activeThreadId).then((loadedAgents) => {
        if (loadedAgents.length > 0) {
          useAppStore.setState((state) => {
            const existingAgentIds = new Set(state.agents.map(a => a.id));
            const newAgents = loadedAgents.filter(a => !existingAgentIds.has(a.id));
            if (newAgents.length === 0) return state;
            return {
              agents: [...state.agents, ...newAgents]
            };
          });
        }
      });
    }
  }, [activeThreadId, loadAgents]);

  return {
    agents,
    activeAgentId,
    activeAgent,
    agentTree,
    loadAgents,
    focusAgent,
    getAgentContextUsage,
    getSubagentIds
  };
}
