/** @jsxImportSource @opentui/react */
import React, { useMemo } from 'react';
import { marked, type Token, type Tokens } from 'marked';
import { SyntaxStyle } from '@opentui/core';

const defaultSyntaxStyle = SyntaxStyle.create();

/**
 * Renders markdown content into OpenTUI components.
 */
export function MarkdownRenderer({ content }: { content: string }) {
  const tokens = useMemo(() => marked.lexer(content), [content]);

  const renderToken = (token: Token, index: number): React.ReactNode => {
    switch (token.type) {
      case 'heading':
        return (
          <box key={index} marginBottom={1}>
            <text fg="#7aa2f7"><b>{'#'.repeat(token.depth) + ' ' + token.text}</b></text>
          </box>
        );

      case 'paragraph':
        return (
          <box key={index} marginBottom={1}>
            <text>{renderInlineTokens(token.tokens)}</text>
          </box>
        );

      case 'blockquote':
        return (
          <box key={index} paddingLeft={1} marginBottom={1} flexDirection="row">
            <box width={1} backgroundColor="#414868" marginRight={1} />
            <box flexGrow={1} flexDirection="column">{token.tokens?.map((t, i) => renderToken(t, i))}</box>
          </box>
        );

      case 'list':
        return (
          <box key={index} marginBottom={1} flexDirection="column">
            {token.items.map((item: Tokens.ListItem, i: number) => (
              <box key={i} flexDirection="row" marginBottom={0}>
                <text fg="#e0af68">{item.task ? (item.checked ? ' [x] ' : ' [ ] ') : ' • '}</text>
                <box flexGrow={1} flexDirection="column">{item.tokens.map((t, j) => renderToken(t, j))}</box>
              </box>
            ))}
          </box>
        );

      case 'code':
        return (
          <box key={index} marginBottom={1}>
            <code content={token.text} filetype={token.lang || 'text'} syntaxStyle={defaultSyntaxStyle} />
          </box>
        );

      case 'hr':
        return (
          <box key={index} height={1} marginBottom={1}>
            <text fg="#414868">{'─'.repeat(20)}</text>
          </box>
        );

      case 'space':
        return null;

      default:
        if ('tokens' in token && token.tokens) {
          return (
            <box key={index} marginBottom={1}>
              <text>{renderInlineTokens(token.tokens)}</text>
            </box>
          );
        }
        return (
          <box key={index} marginBottom={1}>
            <text>{token.raw}</text>
          </box>
        );
    }
  };

  const renderInlineTokens = (tokens: Token[] | undefined): React.ReactNode[] => {
    if (!tokens) return [];
    return tokens.map((token, i) => {
      switch (token.type) {
        case 'strong':
          return <b key={i}>{renderInlineTokens(token.tokens)}</b>;
        case 'em':
          return <i key={i}>{renderInlineTokens(token.tokens)}</i>;
        case 'codespan':
          return <span key={i} bg="#24283b" fg="#9ece6a">{' ' + token.text + ' '}</span>;
        case 'link':
          return <a key={i} href={token.href}>{renderInlineTokens(token.tokens)}</a>;
        case 'text':
          if ('tokens' in token && token.tokens) {
            return <span key={i}>{renderInlineTokens(token.tokens)}</span>;
          }
          return <span key={i}>{(token as Tokens.Text).text}</span>;
        case 'br':
          return <br />;
        default:
          return <span key={i}>{token.raw}</span>;
      }
    });
  };

  return (
    <box flexDirection="column">
      {tokens.map((token, i) => renderToken(token, i))}
    </box>
  );
}
