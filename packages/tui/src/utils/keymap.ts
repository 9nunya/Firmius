export interface KeyMapping {
  key: string;
  ctrl?: boolean;
  meta?: boolean;
  shift?: boolean;
  action: string;
  description: string;
}

export const GLOBAL_KEYMAP: KeyMapping[] = [
  { key: "j", action: "TOGGLE_JUMP_MODE", description: "Enter Jump Mode (scroll to block)" },
  { key: "g", action: "TOGGLE_JUMP_MODE", description: "Enter Jump Mode (scroll to block)" },
  { key: "f", ctrl: true, action: "OPEN_FLEET_MODAL", description: "Open Fleet status modal" },
  { key: "tab", ctrl: true, action: "NEXT_AGENT", description: "Switch to next agent perspective" },
  { key: "1", ctrl: true, action: "SWITCH_AGENT_1", description: "Switch to Agent 1" },
  { key: "2", ctrl: true, action: "SWITCH_AGENT_2", description: "Switch to Agent 2" },
  { key: "3", ctrl: true, action: "SWITCH_AGENT_3", description: "Switch to Agent 3" },
  { key: "4", ctrl: true, action: "SWITCH_AGENT_4", description: "Switch to Agent 4" },
  { key: "5", ctrl: true, action: "SWITCH_AGENT_5", description: "Switch to Agent 5" },
  { key: "6", ctrl: true, action: "SWITCH_AGENT_6", description: "Switch to Agent 6" },
  { key: "7", ctrl: true, action: "SWITCH_AGENT_7", description: "Switch to Agent 7" },
  { key: "8", ctrl: true, action: "SWITCH_AGENT_8", description: "Switch to Agent 8" },
  { key: "9", ctrl: true, action: "SWITCH_AGENT_9", description: "Switch to Agent 9" },
  { key: "escape", action: "CANCEL", description: "Cancel current mode or close modal" },
];