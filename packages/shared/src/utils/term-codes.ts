
export function getTerminalSequence(keybind: string): string {
  const input = keybind.trim().toLowerCase();

  const baseMap: Record<string, string> = {
    'enter': '\r', 'tab': '\t', 'esc': '\x1b', 'space': ' ',
    'backspace': '\x7f', 'delete': '\x1b[3~', 'insert': '\x1b[2~',
    'home': '\x1b[H', 'end': '\x1b[F', 'pageup': '\x1b[5~', 'pagedown': '\x1b[6~'
  };
  if (baseMap[input]) return baseMap[input];

  const parts = input.split('+');
  const key = parts.pop()!;
  const isCtrl = parts.includes('ctrl');
  const isAlt = parts.includes('alt');
  const isShift = parts.includes('shift');

  const arrows: Record<string, string> = { up: 'A', down: 'B', right: 'C', left: 'D' };
  if (arrows[key]) {
    if (!isCtrl && !isAlt && !isShift) return `\x1b[${arrows[key]}`;

    let mod = 1;
    if (isShift) mod += 1;
    if (isAlt) mod += 2;
    if (isCtrl) mod += 4;
    return `\x1b[1;${mod}${arrows[key]}`;
  }

  if (/^f[1-9][0-2]?$/.test(key)) {
    const fIdx = parseInt(key.substring(1));
    if (fIdx <= 4 && !isCtrl && !isAlt && !isShift) {
      return `\x1bO${String.fromCharCode(79 + fIdx)}`;
    }
    const fCodes: Record<number, string> = { 5: '15', 6: '17', 7: '18', 8: '19', 9: '20', 10: '21', 11: '23', 12: '24' };
    const code = fCodes[fIdx] || (fIdx < 5 ? (10 + fIdx).toString() : '20');

    let mod = 1;
    if (isShift) mod += 1;
    if (isAlt) mod += 2;
    if (isCtrl) mod += 4;

    return mod === 1 ? `\x1b[${code}~` : `\x1b[${code};${mod}~`;
  }

  if (isCtrl && key.length === 1) {
    return String.fromCharCode(key.charCodeAt(0) - 96);
  }

  if (isAlt) {
    return '\x1b' + (baseMap[key] || key);
  }

  return key;
}

export const INFO = '\x1b[1;36m';
export const WARN = '\x1b[1;33m';
export const ERROR = '\x1b[1;31m';
export const DEBUG = '\x1b[1;90m';
export const THINKING = '\x1b[1;38;5;245m';
export const CONTENT = '\x1b[1;38;5;39m';
export const TOOL_START = '\x1b[1;38;5;214m';
export const TOOL_SUCCESS = '\x1b[1;32m';
export const TOOL_ERROR = '\x1b[1;31m';
export const RESET = '\x1b[0m';
export const DIM = '\x1b[2m';
