"use client";

import React from "react";
import ReactMarkdown from "react-markdown";
import remarkGfm from "remark-gfm";
import rehypeHighlight from "rehype-highlight";
import { cn } from "../../lib/utils";

interface MarkdownContentProps {
  content: string;
  className?: string;
  isStreaming?: boolean;
}

// Filter out termination signals (handles partial markers like >>>DONE<< or >>DONE<<<)
const DONE_MARKER = ">>>DONE<<<";
const START_MARKER = ">>>START<<<";
// Partial markers: any combination of 1-3 > followed by DONE followed by 1-3 <
const PARTIAL_DONE_REGEX = />{1,3}DONE<{1,3}/g;
const PARTIAL_START_REGEX = />{1,3}START<{1,3}/g;

function filterTerminationSignals(content: string): string {
  return content
    .replace(new RegExp(DONE_MARKER, "g"), "")
    .replace(new RegExp(START_MARKER, "g"), "")
    .replace(PARTIAL_DONE_REGEX, "")
    .replace(PARTIAL_START_REGEX, "")
    .trim();
}

// Memoized markdown content to prevent re-renders during streaming
// Only re-renders when content changes by 50+ chars or when streaming completes
export const MarkdownContent = React.memo(function MarkdownContent({ 
  content, 
  className,
  isStreaming 
}: MarkdownContentProps) {
  const filteredContent = filterTerminationSignals(content);
  
  return (
    <div className={cn("markdown-content max-w-full overflow-hidden", className)}>
      <ReactMarkdown
        remarkPlugins={[remarkGfm]}
        rehypePlugins={[rehypeHighlight]}
        components={{
          pre: ({ ...props }) => (
            <pre className="overflow-x-auto rounded-lg bg-black/50 p-4 my-2 border border-border/30" {...props} />
          ),
          code: ({ className, children, ...props }) => {
            const match = /language-(\w+)/.exec(className || "");
            return match ? (
              <code className={cn("block text-sm", className)} {...props}>
                {children}
              </code>
            ) : (
              <code className="rounded bg-muted px-1 py-0.5 text-xs font-mono" {...props}>
                {children}
              </code>
            );
          },
          p: ({ children }) => <p className="mb-2 last:mb-0 leading-relaxed">{children}</p>,
          a: ({ children, href }) => (
            <a href={href} target="_blank" rel="noopener noreferrer" className="text-primary hover:underline">
              {children}
            </a>
          ),
          ul: ({ children }) => <ul className="list-disc pl-5 mb-2">{children}</ul>,
          ol: ({ children }) => <ol className="list-decimal pl-5 mb-2">{children}</ol>,
          li: ({ children }) => <li className="mb-1">{children}</li>,
          h1: ({ children }) => <h1 className="text-xl font-bold mb-4 mt-6 first:mt-0">{children}</h1>,
          h2: ({ children }) => <h2 className="text-lg font-bold mb-3 mt-5 first:mt-0">{children}</h2>,
          h3: ({ children }) => <h3 className="text-base font-bold mb-2 mt-4 first:mt-0">{children}</h3>,
          blockquote: ({ children }) => (
            <blockquote className="border-l-4 border-primary/30 pl-4 py-1 italic mb-2 text-muted-foreground">
              {children}
            </blockquote>
          ),
          table: ({ children }) => (
            <div className="overflow-x-auto mb-4">
              <table className="min-w-full divide-y divide-border border">{children}</table>
            </div>
          ),
          th: ({ children }) => <th className="px-3 py-2 bg-muted/50 text-left text-xs font-semibold">{children}</th>,
          td: ({ children }) => <td className="px-3 py-2 border-t text-sm">{children}</td>,
        }}
      >
        {filteredContent}
      </ReactMarkdown>
    </div>
  );
}, (prevProps, nextProps) => {
  // Custom comparison: skip re-render during streaming unless significant change
  if (prevProps.isStreaming && nextProps.isStreaming) {
    // During streaming, only re-render every 100 chars to reduce CPU load
    const lengthDiff = Math.abs(nextProps.content.length - prevProps.content.length);
    if (lengthDiff < 100) {
      return true; // Skip re-render
    }
  }
  // Always re-render when streaming completes or content changes significantly
  return prevProps.content === nextProps.content && prevProps.isStreaming === nextProps.isStreaming;
});
