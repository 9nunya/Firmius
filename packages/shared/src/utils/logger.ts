import { stdout, stderr } from 'node:process';
import { isatty } from 'node:tty';

import { INFO as INFO_COLOR, WARN as WARN_COLOR, ERROR as ERROR_COLOR, DEBUG as DEBUG_COLOR, RESET } from './term-codes';

enum LogLevel {
  DEBUG = 0,
  INFO = 1,
  WARN = 2,
  ERROR = 3
}

class Logger {
  private static instance: Logger;
  private level: LogLevel;
  private useColors: boolean;

  private constructor() {
    const envLevel = process.env.FIRMIUS_LOG_LEVEL?.toUpperCase();
    this.level = envLevel !== undefined ? LogLevel[envLevel as keyof typeof LogLevel] ?? LogLevel.INFO : LogLevel.INFO;
    this.useColors = isatty(1) && isatty(2);
  }

  static getInstance(): Logger {
    if (!Logger.instance) {
      Logger.instance = new Logger();
    }
    return Logger.instance;
  }

  private formatMessage(level: string, color: string, message: string): string {
    const timestamp = (performance.now() / 1000).toFixed(6);
    const callerInfo = this.getCallerInfo();
    const levelStr = this.useColors ? `${color}[${level}]${RESET}` : `[${level}]`;
    return `[${timestamp}] ${levelStr} ${callerInfo} ${message}\n`;
  }

  private getCallerInfo(): string {
    const stack = new Error().stack;
    if (!stack) return '';

    const lines = stack.split('\n');
    for (const line of lines) {
      if (line.includes('src/') && !line.includes('Logger.ts')) {
        const match = line.match(/src\/(.+?):(\d+)/);
        if (match) {
          const file = match[1];
          const lineNum = match[2];
          return this.useColors ? `\x1b[90m[${file}:${lineNum}]\x1b[0m` : `[${file}:${lineNum}]`;
        }
      }
    }
    return '';
  }

  private formatStackTrace(stack: string): string {
    const lines = stack.split('\n');
    const frames: { file: string; line: string; func?: string }[] = [];

    for (const line of lines) {
      if (line && line.includes('src/') && !line.includes('Logger.ts') && !line.includes('node:') && !line.includes('internal/')) {
        const fileMatch = line.match(/src\/(.+?):(\d+):/);

        if (fileMatch && fileMatch[1] && fileMatch[2]) {
          const file = fileMatch[1];
          const lineNum = fileMatch[2];
          const funcMatch = line.match(/at\s+(?:async\s+)?(\w+|<anonymous>)\s+\(([^)]+)\)/);
          const func = funcMatch ? funcMatch[1] : undefined;
          frames.push({ file, line: lineNum, func });
        }
      }
    }

    if (frames.length === 0) {
      return '';
    }

    const gray = this.useColors ? '\x1b[90m' : '';
    const reset = this.useColors ? RESET : '';
    const arrow = this.useColors ? `${gray}←${reset}` : '←';

    let result = `\n${gray}Stack Trace:${reset}\n`;

    const maxPadding = Math.max(...frames.map(f => `${f.file}:${f.line}`.length));

    for (let i = 0; i < frames.length; i++) {
      const frame = frames[i];
      if (!frame) continue;
      const { file, line, func } = frame;
      const indent = '  '.repeat(i);
      const fileLine = `${file}:${line}`;
      const padding = ' '.repeat(maxPadding - fileLine.length + 2);
      const funcPart = func ? ` ${arrow} ${func}()` : '';
      result += `${indent}${fileLine}${padding}${funcPart}\n`;
    }

    return result;
  }

  debug(message: string): void {
    if (this.level <= LogLevel.DEBUG) {
      stdout.write(this.formatMessage('DEBUG', DEBUG_COLOR, message));
    }
  }

  info(message: string): void {
    if (this.level <= LogLevel.INFO) {
      stdout.write(this.formatMessage('INFO', INFO_COLOR, message));
    }
  }

  warn(message: string): void {
    if (this.level <= LogLevel.WARN) {
      stderr.write(this.formatMessage('WARN', WARN_COLOR, message));
    }
  }

  error(messageOrError: string | Error, error?: Error): void {
    if (this.level > LogLevel.ERROR) return;

    if (error !== undefined) {
      const message = messageOrError as string;
      const timestamp = (performance.now() / 1000).toFixed(6);
      const callerInfo = this.getCallerInfo();
      const levelStr = this.useColors ? `${ERROR_COLOR}[ERROR]${RESET}` : `[ERROR]`;
      const errorInfo = `${error.name}: ${error.message}`;
      const formatted = `[${timestamp}] ${levelStr} ${callerInfo} ${message}\n${errorInfo}${this.formatStackTrace(error.stack ?? '')}`;
      stderr.write(formatted + '\n');
    } else if (messageOrError instanceof Error) {
      const err = messageOrError;
      const timestamp = (performance.now() / 1000).toFixed(6);
      const levelStr = this.useColors ? `${ERROR_COLOR}[ERROR]${RESET}` : `[ERROR]`;
      const errorMessage = `${err.name}: ${err.message}`;
      const formatted = `[${timestamp}] ${levelStr} ${errorMessage}${this.formatStackTrace(err.stack ?? '')}`;
      stderr.write(formatted + '\n');
    } else {
      const message = messageOrError as string;
      stderr.write(this.formatMessage('ERROR', ERROR_COLOR, message));
    }
  }
}

export const logger = Logger.getInstance();
