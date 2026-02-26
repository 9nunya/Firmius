"use client";

import React from "react";
import type { Message } from "../../types";
import { Terminal } from "lucide-react";

import DiagnosticsBlock from "./DiagnosticsBlock";
import FileQueryBlock from "./FileQueryBlock";
import FileReadBlock from "./FileReadBlock";
import FileEditBlock from "./FileEditBlock";
import FileManageBlock from "./FileManageBlock";
import TodoBlock from "./TodoBlock";
import ProcessControlBlock from "./ProcessControlBlock";
import ProcessExecuteBlock from "./ProcessExecuteBlock";
import WebAccessBlock from "./WebAccessBlock";
import GitOpsBlock from "./GitOpsBlock";
import GitWorktreeBlock from "./GitWorktreeBlock";
import GitMergeBlock from "./GitMergeBlock";
import LSPBlock from "./LSPBlock";
import CompactionManageBlock from "./CompactionManageBlock";
import ContextManageBlock from "./ContextManageBlock";
import ReportProgressBlock from "./ReportProgressBlock";
import SubagentToolBlock from "./SubagentToolBlock";

function ToolCallStream({ toolCalls }: { toolCalls: Message["toolCalls"] }) {
  if (!toolCalls) return null;

  const filteredTools = toolCalls.filter(
    (tc) => tc.name !== "respond",
  );
  if (filteredTools.length === 0) return null;

  return (
    <div className="flex flex-col gap-2">
      <div className="flex items-center gap-2 text-[10px] font-bold uppercase tracking-widest text-muted-foreground/30 mb-1 ml-1">
        <Terminal className="h-3 w-3" />
        Capabilities
      </div>
      <div className="flex flex-col gap-2">
        {filteredTools.map((tc, i) => {
          const name = tc.name || "unknown";
          const typedTc = { ...tc, args: tc.args as Record<string, unknown> | undefined };
          const key = typedTc.callId || `anon-${i}-${typedTc.name}`;

          switch (name) {
            case "agent_delegate":
              return <SubagentToolBlock key={key} toolCall={typedTc} />;

            case "subagent_wait":
            case "subagent_poll":
            case "subagent_nudge":
            case "subagent_status":
            case "subagent_kill":
              return <SubagentToolBlock key={key} toolCall={typedTc} />;

            case "file_read":
              return <FileReadBlock key={key} toolCall={typedTc} />;

            case "file_edit":
            case "edit_file":
            case "write_file":
            case "edit":
            case "write":
              return <FileEditBlock key={key} toolCall={typedTc} />;

            case "file_manage":
              return <FileManageBlock key={key} toolCall={typedTc} />;

            case "todo":
              return <TodoBlock key={key} toolCall={typedTc} />;

            case "process_control":
            case "process_wait":
              return <ProcessControlBlock key={key} toolCall={typedTc} />;

            case "process_execute":
              return <ProcessExecuteBlock key={key} toolCall={typedTc} />;

            case "web_access":
              return <WebAccessBlock key={key} toolCall={typedTc} />;

            case "git_ops":
              return <GitOpsBlock key={key} toolCall={typedTc} />;

            case "git_worktree":
              return <GitWorktreeBlock key={key} toolCall={typedTc} />;

            case "git_merge":
              return <GitMergeBlock key={key} toolCall={typedTc} />;

            case "lsp_dependencies":
            case "lsp_explore":
            case "lsp_find":
            case "lsp_inspect":
            case "lsp_lookup":
            case "code_execute":
              return <LSPBlock key={key} toolCall={typedTc} />;

            case "compaction_manage":
              return (
                <CompactionManageBlock key={key} toolCall={typedTc} />
              );

            case "context_manage":
              return <ContextManageBlock key={key} toolCall={typedTc} />;

            case "report_progress":
              return <ReportProgressBlock key={key} toolCall={typedTc} />;

            case "file_query":
            case "search_files":
              return <FileQueryBlock key={key} toolCall={typedTc} />;

            case "diagnostics":
              return <DiagnosticsBlock key={key} toolCall={typedTc} />;

            case "unknown":
              return (
                <div
                  key={key}
                  className="text-xs text-muted-foreground p-2"
                >
                  Unknown tool
                </div>
              );

            default:
              return (
                <div
                  key={key}
                  className="text-xs text-muted-foreground p-2"
                >
                  Tool: {name}
                </div>
              );
          }
        })}
      </div>
    </div>
  );
}

export default ToolCallStream;
