/** @jsxImportSource @opentui/react */
import { useRef, useEffect, useMemo } from 'react';
import useAppStore, { selectFilteredMessages } from '../store/appStore';
import { BlockFactory } from './blocks/BlockFactory';
import { ResponseBlock } from './blocks/ResponseBlock';
import { useShallow } from 'zustand/react/shallow';
import type { Message } from '@firmius/shared/api';

interface Turn {
  id: string;
  userMessage?: Message;
  workspace: Message[];
  response?: Message;
  isComplete: boolean;
}

/**
 * LogStream: The primary message display area.
 */
export function LogStream() {
  const messages = useAppStore(useShallow(selectFilteredMessages));
  const collapsedTurns = useAppStore((state) => state.collapsedTurns);
  const toggleTurnCollapse = useAppStore((state) => state.toggleTurnCollapse);
  const setJumpCodes = useAppStore((state) => state.setJumpCodes);
  const scrollRef = useRef<any>(null);

  // Group messages into Turns
  const turns = useMemo(() => {
    const result: Turn[] = [];
    let currentTurn: Turn | null = null;

    messages.forEach((msg) => {
      if (msg.isUser) {
        if (currentTurn) result.push(currentTurn);
        currentTurn = {
          id: `turn-${msg.sequence}`,
          userMessage: msg,
          workspace: [],
          isComplete: false,
        };
      } else {
        if (!currentTurn) {
          currentTurn = {
            id: `turn-agent-${msg.sequence}`,
            workspace: [],
            isComplete: false,
          };
        }

        if (msg.type === "response" && !msg.isStreaming) {
          currentTurn.response = msg;
          currentTurn.isComplete = true;
          result.push(currentTurn);
          currentTurn = null;
        } else if (msg.type === "response" && msg.isStreaming) {
          currentTurn.response = msg;
        } else {
          currentTurn.workspace.push(msg);
        }
      }
    });

    if (currentTurn) result.push(currentTurn);
    return result;
  }, [messages]);

  const jumpCodeMap = useMemo(() => {
    const map: Record<string, string> = {};
    const alphabet = "abcdefghijklmnopqrstuvwxyz";
    messages.slice(-26 * 26).forEach((msg, i) => {
      const first = alphabet[Math.floor(i / 26)];
      const second = alphabet[i % 26];
      const code = `${first}${second}`;
      map[msg.sequence.toString()] = code;
    });
    return map;
  }, [messages]);

  useEffect(() => {
    const currentCodes = useAppStore.getState().jumpCodes;
    const codesToSequences: Record<string, number> = {};
    Object.entries(jumpCodeMap).forEach(([seq, code]) => {
      codesToSequences[code] = parseFloat(seq);
    });
    if (JSON.stringify(currentCodes) !== JSON.stringify(codesToSequences)) {
      setTimeout(() => setJumpCodes(codesToSequences), 0);
    }
  }, [jumpCodeMap, setJumpCodes]);

  useEffect(() => {
    if (scrollRef.current) {
        scrollRef.current.scrollToBottom?.();
    }
  }, [messages]);

  if (turns.length === 0) {
    return (
      <box width="100%" height="100%" justifyContent="center" alignItems="center" flexDirection="column">
        <text fg="#444444">No active thread selected.</text>
        <text fg="#333333">Press Ctrl+P to create a new thread.</text>
      </box>
    );
  }

  return (
    <scrollbox 
      ref={scrollRef} 
      width="100%" 
      height="100%" 
      flexDirection="column"
      stickyScroll={true}
      stickyStart="bottom"
    >
      <box flexDirection="column" width="100%" padding={1}>
        {turns.map((turn) => {
          const isCollapsed = collapsedTurns.has(turn.id);
          
          // Work is anything in workspace OR work inside the response message
          const hasWorkspaceWork = turn.workspace.length > 0;
          const hasResponseWork = !!(turn.response && (turn.response.thinking || (turn.response.toolCalls && turn.response.toolCalls.length > 0)));
          const hasWork = hasWorkspaceWork || hasResponseWork;

          return (
            <box key={turn.id} flexDirection="column" marginBottom={2}>
              {/* User Message */}
              {turn.userMessage && (
                <BlockFactory 
                  message={turn.userMessage} 
                  jumpCode={jumpCodeMap[turn.userMessage.sequence.toString()]} 
                />
              )}
              
              {/* Workspace (Collapsible) */}
              {hasWork && (
                <box flexDirection="column" marginTop={1} marginBottom={1}>
                  <box flexDirection="row" alignItems="center" marginBottom={1}>
                    <box height={1} flexGrow={1} backgroundColor="#1A1A1A" marginRight={1} />
                    <box onMouseDown={() => toggleTurnCollapse(turn.id)} flexDirection="row">
                        <text fg="#444444"><b>{" AGENT WORK "}</b></text>
                        <text fg="#00FF00">{isCollapsed ? "[+ SHOW]" : "[- HIDE]"}</text>
                    </box>
                    <box height={1} flexGrow={1} backgroundColor="#1A1A1A" marginLeft={1} />
                  </box>
                  
                  {!isCollapsed && (
                    <box flexDirection="row" paddingLeft={2}>
                      <box width={1} backgroundColor="#1A1A1A" marginRight={1} />
                      <box flexDirection="column" flexGrow={1}>
                        {/* 1. Workspace messages (thinking, tools, monologues) */}
                        {turn.workspace.map(msg => (
                          <BlockFactory key={msg.sequence} message={msg} jumpCode={jumpCodeMap[msg.sequence.toString()]} />
                        ))}
                        
                        {/* 2. Extra work inside the response message itself */}
                        {turn.response && (
                            <BlockFactory 
                                key={`resp-work-${turn.response.sequence}`} 
                                message={{
                                    ...turn.response,
                                    content: "" // Don't render content twice, only the work parts
                                }} 
                            />
                        )}
                      </box>
                    </box>
                  )}
                </box>
              )}

              {/* Final Response (CONTENT ONLY) */}
              {turn.response && (
                <ResponseBlock 
                    content={typeof turn.response.content === 'string' ? turn.response.content : JSON.stringify(turn.response.content)} 
                    isStreaming={turn.response.isStreaming}
                />
              )}
            </box>
          );
        })}
        <box height={2} />
      </box>
    </scrollbox>
  );
}
