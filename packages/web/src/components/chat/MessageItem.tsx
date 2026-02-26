import React, { useState } from 'react';
import ReactMarkdown from 'react-markdown';
import remarkGfm from 'remark-gfm';
import { User, Trash2, Edit3, Check, X, GitBranch } from 'lucide-react';
import useAppStore from '@/stores/app-store';
import { cn } from '@/lib/utils';
import type { Message } from '@/types';

interface MessageItemProps {
  message: Message;
  agentName?: string;
}

export default function MessageItem({ message, agentName }: MessageItemProps) {
  const { editMessage, deleteMessage, branchThread } = useAppStore();
  const [isEditing, setIsEditing] = useState(false);
  const [isBranching, setIsBranching] = useState(false);
  const [editContent, setEditContent] = useState(message.content);

  const formatTimestamp = (date: Date) => {
    const now = new Date();
    const diff = now.getTime() - date.getTime();
    const seconds = Math.floor(diff / 1000);
    const minutes = Math.floor(seconds / 60);
    const hours = Math.floor(minutes / 60);
    const days = Math.floor(hours / 24);

    if (seconds < 60) return 'Just now';
    if (minutes < 60) return `${minutes}m ago`;
    if (hours < 24) return `${hours}h ago`;
    if (days < 7) return `${days}d ago`;

    return date.toLocaleDateString();
  };

  const handleSave = () => {
    // If editContent is array (multimodal), we currently serialize it or extract text.
    // For simplicity, we assume text edits for now.
    // Ideally branchThread/editMessage should support array content.
    const contentToSave = typeof editContent === 'string' ? editContent : JSON.stringify(editContent);

    if (isBranching) {
      branchThread(message.sequence, contentToSave);
    } else {
      editMessage(message.sequence, contentToSave);
    }
    setIsEditing(false);
    setIsBranching(false);
  };

  const handleCancel = () => {
    setEditContent(message.content);
    setIsEditing(false);
    setIsBranching(false);
  };

  const handleBranchClick = () => {
    setIsBranching(true);
    setIsEditing(true);
  };

  const handleDelete = () => {
    deleteMessage(message.sequence);
  };

  return (
    <div className="group flex gap-3 p-4 hover:bg-gray-50 dark:hover:bg-gray-900/30 transition-colors">
      {message.isUser ? (
        <div className="flex-shrink-0">
          <div className="w-8 h-8 rounded-full bg-blue-500 flex items-center justify-center text-white">
            <User size={16} />
          </div>
        </div>
      ) : (
        <div className="flex-shrink-0">
          <div className="w-8 h-8 rounded-full bg-purple-500 flex items-center justify-center text-white font-semibold text-sm">
            {agentName?.charAt(0).toUpperCase() || 'A'}
          </div>
        </div>
      )}

      <div className="flex-1 min-w-0">
        <div className="flex items-center gap-2 mb-1">
          <span className="font-semibold text-sm text-gray-900 dark:text-gray-100">
            {message.isUser ? 'You' : agentName || 'Agent'}
          </span>
          <span className="text-xs text-gray-500 dark:text-gray-400">
            {formatTimestamp(message.timestamp)}
          </span>
          {message.isUser && (
            <div className="flex items-center gap-1 opacity-0 group-hover:opacity-100 transition-opacity">
              <button
                onClick={() => setIsEditing(true)}
                className="p-1 hover:bg-gray-200 dark:hover:bg-gray-700 rounded transition-colors"
                aria-label="Edit message"
              >
                <Edit3 size={14} className="text-gray-600 dark:text-gray-400" />
              </button>
              <button
                onClick={handleBranchClick}
                className="p-1 hover:bg-gray-200 dark:hover:bg-gray-700 rounded transition-colors"
                aria-label="Branch from here"
              >
                <GitBranch size={14} className="text-gray-600 dark:text-gray-400" />
              </button>
              <button
                onClick={handleDelete}
                className="p-1 hover:bg-red-100 dark:hover:bg-red-900/30 rounded transition-colors"
                aria-label="Delete message"
              >
                <Trash2 size={14} className="text-gray-600 dark:text-gray-400 hover:text-red-600 dark:hover:text-red-400" />
              </button>
            </div>
          )}
        </div>

        {isEditing && message.isUser ? (
          <div className="space-y-2">
            <textarea
              value={typeof editContent === 'string' ? editContent : JSON.stringify(editContent)}
              onChange={(e) => setEditContent(e.target.value)}
              className="w-full min-h-[100px] p-3 rounded-lg border border-gray-300 dark:border-gray-600 bg-white dark:bg-gray-800 text-gray-900 dark:text-gray-100 focus:outline-none focus:ring-2 focus:ring-blue-500 dark:focus:ring-blue-400 resize-y"
              autoFocus
            />
            <div className="flex gap-2">
              <button
                onClick={handleSave}
                className={cn(
                  "flex items-center gap-1 px-3 py-1.5 text-white text-sm font-medium rounded-lg transition-colors",
                  isBranching ? "bg-amber-500 hover:bg-amber-600" : "bg-blue-500 hover:bg-blue-600"
                )}
              >
                {isBranching ? <GitBranch size={14} /> : <Check size={14} />}
                {isBranching ? 'Branch' : 'Save'}
              </button>
              <button
                onClick={handleCancel}
                className="flex items-center gap-1 px-3 py-1.5 bg-gray-200 dark:bg-gray-700 hover:bg-gray-300 dark:hover:bg-gray-600 text-gray-900 dark:text-gray-100 text-sm font-medium rounded-lg transition-colors"
              >
                <X size={14} />
                Cancel
              </button>
            </div>
          </div>
        ) : (
          <div className="prose prose-sm dark:prose-invert max-w-none">
            <ReactMarkdown remarkPlugins={[remarkGfm]}>
              {typeof message.content === 'string' ? message.content : JSON.stringify(message.content)}
            </ReactMarkdown>
          </div>
        )}
      </div>
    </div>
  );
}
