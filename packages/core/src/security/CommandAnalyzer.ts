/**
 * Command Security Analyzer
 *
 * Parses shell commands into structured data and applies security policies.
 * Replaces regex-based blocking with semantic analysis.
 */

export enum SecuritySeverity {
  CRITICAL = "CRITICAL",
  HIGH = "HIGH",
  MEDIUM = "MEDIUM",
  LOW = "LOW",
  ALLOWED = "ALLOWED"
}

export enum PathIntent {
  READ = "READ",
  WRITE = "WRITE",
  EXECUTE = "EXECUTE",
  DELETE = "DELETE",
  UNKNOWN = "UNKNOWN"
}

export enum PathType {
  PROJECT = "PROJECT",
  TEMP = "TEMP",
  SYSTEM = "SYSTEM",
  SENSITIVE = "SENSITIVE",
  UNKNOWN = "UNKNOWN"
}

export enum OperationType {
  GIT_CHECKOUT = "GIT_CHECKOUT",
  GIT_RESET_HARD = "GIT_RESET_HARD",
  GIT_CLONE = "GIT_CLONE",
  SED_EDIT = "SED_EDIT",
  SHELL_INJECTION = "SHELL_INJECTION",
  DESTRUCTIVE_DELETE = "DESTRUCTIVE_DELETE",
  DEPENDENCY_INSTALL = "DEPENDENCY_INSTALL",
  FILE_CREATE = "FILE_CREATE",
  FILE_READ = "FILE_READ",
  DIRECTORY_CREATE = "DIRECTORY_CREATE",
  COMMAND_CHAINING = "COMMAND_CHAINING",
  HEAD_TAIL_STREAM = "HEAD_TAIL_STREAM"
}

export interface CommandPath {
  path: string;
  intent: PathIntent;
  type: PathType;
  isGlob: boolean;
}

export interface ParsedCommand {
  command: string;
  subcommand?: string;
  args: string[];
  flags: Map<string, string | boolean>;
  paths: CommandPath[];
  detectedOperations: OperationType[];
  hasShellInjection: boolean;
  hasCommandChaining: boolean;
  raw: string;
}

export interface CommandAnalysis {
  allowed: boolean;
  severity: SecuritySeverity;
  reason: string;
  redirect?: string;
  details?: {
    parsedCommand: ParsedCommand;
    blockedOperations: OperationType[];
    riskyPaths: CommandPath[];
  };
}

// ============================================================================
// COMMAND PARSING
// ============================================================================

const SHELL_INJECTION_PATTERNS = [
  /`[^`]*`/,           // Backtick command substitution
  /\$\([^)]*\)/,       // $(command) substitution
  /\$\{[^}]*\}/,       // ${variable} expansion
  /;&&/,               // Command chaining with ;&&
  /\|&/,               // Pipe to background/chain
];

const TEMP_PATHS = ["/tmp", "/var/tmp", "/temp", "~/.cache", "${TMPDIR}"];
const SYSTEM_PATHS = ["/etc", "/usr", "/bin", "/sbin", "/lib", "/opt"];
const SENSITIVE_PATHS = ["~/.ssh", "~/.aws", "~/.config", "~/.npm", "~/.gitconfig"];

/**
 * Tokenizes a shell command respecting quotes
 */
function tokenizeCommand(cmd: string): string[] {
  const tokens: string[] = [];
  let current = "";
  let inSingleQuote = false;
  let inDoubleQuote = false;
  let escapeNext = false;

  for (let i = 0; i < cmd.length; i++) {
    const char = cmd[i];

    if (escapeNext) {
      current += char;
      escapeNext = false;
      continue;
    }

    if (!char) continue;

    if (char === "\\" && !inSingleQuote) {
      escapeNext = true;
      current += char;
      continue;
    }

    if (char === "'" && !inDoubleQuote) {
      inSingleQuote = !inSingleQuote;
      current += char;
      continue;
    }

    if (char === '"' && !inSingleQuote) {
      inDoubleQuote = !inDoubleQuote;
      current += char;
      continue;
    }

    if (char === " " && !inSingleQuote && !inDoubleQuote) {
      if (current) {
        tokens.push(current);
        current = "";
      }
      continue;
    }

    current += char;
  }

  if (current) {
    tokens.push(current);
  }

  return tokens;
}

/**
 * Extracts flags from command arguments
 */
function extractFlags(args: string[]): Map<string, string | boolean> {
  const flags = new Map<string, string | boolean>();

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (!arg) continue;

    if (arg.startsWith("--")) {
      const equalIndex = arg.indexOf("=");
      if (equalIndex > -1) {
        flags.set(arg.slice(2, equalIndex), arg.slice(equalIndex + 1));
      } else {
        const nextArg = args[i + 1];
        if (nextArg && !nextArg.startsWith("-")) {
          flags.set(arg.slice(2), nextArg);
          i++; // Skip next arg as it's the value
        } else {
          flags.set(arg.slice(2), true);
        }
      }
    } else if (arg.startsWith("-") && arg.length > 1) {
      // Short flags (-a, -abc, -a value)
      const chars = arg.slice(1).split("");

      if (chars.length === 1) {
        const nextArg = args[i + 1];
        if (nextArg && !nextArg.startsWith("-")) {
          flags.set(chars[0]!, nextArg);
          i++;
        } else {
          flags.set(chars[0]!, true);
        }
      } else {
        // Multiple short flags like -abc
        for (const char of chars) {
          flags.set(char, true);
        }
      }
    }
  }

  return flags;
}

/**
 * Determines if a path is a glob pattern
 */
function isGlobPattern(path: string): boolean {
  return path.includes("*") || path.includes("?") || path.includes("[");
}

/**
 * Classifies a path by type
 */
function classifyPathType(path: string): PathType {
  const normalized = path.toLowerCase().replace(/^~/, process.env.HOME || "");

  if (TEMP_PATHS.some((temp) => normalized.includes(temp.toLowerCase()))) {
    return PathType.TEMP;
  }

  if (SENSITIVE_PATHS.some((sensitive) => normalized.includes(sensitive.toLowerCase()))) {
    return PathType.SENSITIVE;
  }

  if (SYSTEM_PATHS.some((sys) => normalized.startsWith(sys.toLowerCase()))) {
    return PathType.SYSTEM;
  }

  return PathType.UNKNOWN;
}

/**
 * Checks if a command chain is safe (e.g., "cd /path && npm install")
 * These are allowed even with command chaining
 */
function isSafeDependencyInstallChain(rawCmd: string): boolean {
  // Split by && or ||
  const parts = rawCmd.split(/&&|\|\|/).map(s => s.trim());

  if (parts.length === 0) return false;

  // Get the last command (the actual operation)
  const lastPart = parts[parts.length - 1];
  if (!lastPart) return false;

  const lastTokens = tokenizeCommand(lastPart);
  const lastCommand = lastTokens[0] || "";
  const lastArgs = lastTokens.slice(1);

  // Check if the final command is a safe dependency install
  return isDependencyInstallCommand(lastCommand, lastArgs);
}

/**
 * Parses a command string into structured data
 */
export function parseCommand(rawCmd: string): ParsedCommand {
  const tokens = tokenizeCommand(rawCmd.trim());
  const command = tokens[0] || "";
  const args = tokens.slice(1);
  const flags = extractFlags(args);

  const result: ParsedCommand = {
    command,
    args,
    flags,
    paths: [],
    detectedOperations: [],
    hasShellInjection: false,
    hasCommandChaining: false,
    raw: rawCmd
  };

  // Detect shell injection
  for (const pattern of SHELL_INJECTION_PATTERNS) {
    if (pattern.test(rawCmd)) {
      result.hasShellInjection = true;
      result.detectedOperations.push(OperationType.SHELL_INJECTION);
      break;
    }
  }

  // Detect command chaining - but only block if not a safe dependency install chain
  if (/&&|\|\|/.test(rawCmd)) {
    result.hasCommandChaining = true;
    // Only mark as blocking operation if it's not a safe dependency install
    if (!isSafeDependencyInstallChain(rawCmd)) {
      result.detectedOperations.push(OperationType.COMMAND_CHAINING);
    }
  }

  // Parse subcommand for git
  if (command === "git" && args.length > 0) {
    result.subcommand = args[0];

    if (result.subcommand === "checkout") {
      result.detectedOperations.push(OperationType.GIT_CHECKOUT);
    } else if (result.subcommand === "reset" && flags.has("hard")) {
      result.detectedOperations.push(OperationType.GIT_RESET_HARD);
    } else if (result.subcommand === "clone") {
      result.detectedOperations.push(OperationType.GIT_CLONE);
    }
  }

  // Detect sed
  if (command === "sed") {
    result.detectedOperations.push(OperationType.SED_EDIT);
  }

  // Detect rm -rf /
  if (
    command === "rm" &&
    (flags.has("r") || flags.has("rf") || flags.has("recursive")) &&
    args.some((arg) => arg === "/" || arg.startsWith("/ "))
  ) {
    result.detectedOperations.push(OperationType.DESTRUCTIVE_DELETE);
  }

  // Extract paths from arguments
  for (const arg of args) {
    // Skip options and flags
    if (arg.startsWith("-")) continue;

    // Skip URLs (for git clone, curl, wget)
    if (arg.match(/^https?:\/\//) || arg.match(/^git@/)) continue;

    // Heuristic: paths often contain / or . or are file-like
    if (
      arg.includes("/") ||
      arg.startsWith(".") ||
      arg.startsWith("~") ||
      arg.match(/\.(js|ts|py|go|rs|cpp|c|h|json|yml|yaml|md|txt|sh|bash|zsh)$/)
    ) {
      const intent = inferPathIntent(command, flags, arg, args);
      result.paths.push({
        path: arg,
        intent,
        type: classifyPathType(arg),
        isGlob: isGlobPattern(arg)
      });
    }
  }

  // Detect operation types based on command
  if (isDependencyInstallCommand(command, args)) {
    result.detectedOperations.push(OperationType.DEPENDENCY_INSTALL);
  }

  if (command === "mkdir" || command === "mkdirp") {
    result.detectedOperations.push(OperationType.DIRECTORY_CREATE);
  }

  if (command === "touch" || (command === "echo" && args.includes(">"))) {
    result.detectedOperations.push(OperationType.FILE_CREATE);
  }

  if (command === "head" || command === "tail") {
    result.detectedOperations.push(OperationType.HEAD_TAIL_STREAM);
  }

  if (["cat", "less", "more"].includes(command)) {
    result.detectedOperations.push(OperationType.FILE_READ);
  }

  return result;
}

/**
 * Infers the intent of a path argument
 */
function inferPathIntent(
  command: string,
  flags: Map<string, string | boolean>,
  arg: string,
  allArgs: string[]
): PathIntent {
  const argIndex = allArgs.indexOf(arg);

  // Check for output redirection indicators before this arg
  for (let i = argIndex - 1; i >= 0; i--) {
    const prevArg = allArgs[i];
    if (prevArg === ">" || prevArg === ">>") {
      return PathIntent.WRITE;
    }
    if (prevArg === "<") {
      return PathIntent.READ;
    }
  }

  // Check command-specific patterns
  if (command === "rm" || command === "rmdir") {
    return PathIntent.DELETE;
  }

  if (command === "cp" || command === "mv") {
    // Last arg is usually destination
    const nonFlagArgs = allArgs.filter((a) => !a.startsWith("-"));
    if (nonFlagArgs[nonFlagArgs.length - 1] === arg && nonFlagArgs.length > 1) {
      return PathIntent.WRITE;
    }
    return PathIntent.READ;
  }

  if (command === "touch" || command === "mkdir") {
    return PathIntent.WRITE;
  }

  if (command === "cat" || command === "head" || command === "tail" || command === "less") {
    return PathIntent.READ;
  }

  // Check for executable bit being set
  if (command === "chmod" && (flags.has("x") || flags.has("+x"))) {
    return PathIntent.EXECUTE;
  }

  return PathIntent.UNKNOWN;
}

/**
 * Checks if command is a dependency installer
 */
function isDependencyInstallCommand(command: string, args: string[]): boolean {
  const installCommands = [
    "npm",
    "yarn",
    "pnpm",
    "pip",
    "pip3",
    "gem",
    "bundle",
    "composer",
    "go",
    "cargo",
    "cargo-install",
    "mvn",
    "gradle",
    "apt",
    "apt-get",
    "yum",
    "dnf",
    "brew"
  ];

  if (!installCommands.includes(command)) return false;

  // Check for install subcommand or flag
  const installIndicators = [
    "install",
    "add",
    "get",
    "i",
    "-i",
    "--install"
  ];

  return args.some(
    (arg) =>
      installIndicators.includes(arg) ||
      arg.startsWith("install") ||
      arg === "mod" || // go mod
      arg === "get" // go get
  );
}

// ============================================================================
// SECURITY ANALYSIS
// ============================================================================

interface SecurityRule {
  name: string;
  severity: SecuritySeverity;
  check: (cmd: ParsedCommand) => boolean;
  reason: string;
  redirect?: string;
}

const SECURITY_RULES: SecurityRule[] = [
  // CRITICAL: NEVER ALLOW
  {
    name: "GIT_CHECKOUT",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.detectedOperations.includes(OperationType.GIT_CHECKOUT),
    reason: "git checkout is NEVER allowed under ANY circumstances",
    redirect: "You CANNOT change branches. Work with the current branch only."
  },
  {
    name: "GIT_RESET_HARD",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.detectedOperations.includes(OperationType.GIT_RESET_HARD),
    reason: "git reset --hard will destroy uncommitted work",
    redirect: "Use file editing tools to revert specific changes, not git reset."
  },
  {
    name: "SED_ANY",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.command === "sed",
    reason: "sed is NEVER allowed. It is dangerous and unpredictable",
    redirect: "Use 'file_edit' with operation='replace' for file modifications."
  },
  {
    name: "SHELL_INJECTION",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.hasShellInjection && !isSafeDependencyInstallChain(cmd.raw),
    reason: "Shell injection detected - command substitution or dangerous chaining is forbidden",
    redirect: "Run commands separately. Do not use backticks, $(), or dangerous command chaining."
  },
  {
    name: "COMMAND_CHAINING",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.detectedOperations.includes(OperationType.COMMAND_CHAINING),
    reason: "Command chaining is forbidden except for dependency installs",
    redirect: "For npm/yarn/pip installs, use the full command. For other operations, run separately."
  },
  {
    name: "DESTRUCTIVE_DELETE",
    severity: SecuritySeverity.CRITICAL,
    check: (cmd) => cmd.detectedOperations.includes(OperationType.DESTRUCTIVE_DELETE),
    reason: "Destructive delete of system root or critical paths",
    redirect: "Use 'file_edit' to remove specific files if needed."
  },

  // HIGH: Strong warning, context dependent
  {
    name: "CAT_FILE_READ",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "cat" && cmd.paths.length > 0,
    reason: "cat is blocked for file reading",
    redirect: "Use 'file_read' to observe file contents. It is optimized for context efficiency."
  },
  {
    name: "GREP_SEARCH",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "grep" || cmd.command === "rg",
    reason: "grep is blocked for searching",
    redirect: "Use 'file_query' (operation: 'search') for structured, parsed results."
  },
  {
    name: "FIND_FILES",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "find",
    reason: "find is discouraged for file discovery",
    redirect: "Use 'file_query' (operation: 'list' or 'search') for file operations."
  },
  {
    name: "LS_RECURSIVE",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "ls" && (cmd.flags.has("R") || cmd.flags.has("recursive")),
    reason: "Recursive listing is blocked",
    redirect: "Use 'file_query' (operation: 'list') on specific sub-directories."
  },
  {
    name: "AWK_PROCESSING",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "awk",
    reason: "awk is discouraged",
    redirect: "Use 'file_read' or 'file_query' instead."
  },
  {
    name: "CUT_PROCESSING",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "cut",
    reason: "cut is discouraged",
    redirect: "Use 'file_read' or process output programmatically."
  },
  {
    name: "WC_COUNTING",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "wc",
    reason: "wc is discouraged",
    redirect: "Use 'file_read' with limit parameter instead."
  },
  {
    name: "SORT_PROCESSING",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "sort",
    reason: "sort is discouraged",
    redirect: "Use appropriate file tools."
  },
  {
    name: "LESS_MORE_PAGER",
    severity: SecuritySeverity.HIGH,
    check: (cmd) => cmd.command === "less" || cmd.command === "more",
    reason: "Interactive pagers are blocked",
    redirect: "Use 'file_read' for file reading."
  },

  // MEDIUM: Warning, allowed with monitoring
  {
    name: "HEAD_TAIL_STREAM",
    severity: SecuritySeverity.MEDIUM,
    check: (cmd) =>
      (cmd.command === "head" || cmd.command === "tail") &&
      // Allow if it's on an output file from a SHORT command
      !isShortOutputFile(cmd),
    reason:
      "head/tail is ONLY allowed on SHORT instant commands. NOT on long-running commands or streams",
    redirect:
      "Use head/tail ONLY on SHORT instant commands (< 5 seconds). For file reading, use 'file_read'."
  }
];

/**
 * Heuristic to check if head/tail is being used on output from a short command
 * This is permissive - we allow common patterns that are clearly safe
 */
function isShortOutputFile(cmd: ParsedCommand): boolean {
  // If it's just head/tail on a file path, check if it looks like a log or stream
  const args = cmd.args.filter((a) => !a.startsWith("-"));

  if (args.length === 0) return false; // Reading from stdin - risky

  const target = args[0];

  // Safe patterns: static files, not pipes or process substitution
  if (target?.includes("/var/log")) return false; // Logs are streams
  if (target?.includes(".log")) return false; // Log files
  if (target?.startsWith("/proc")) return false; // Proc fs
  if (target?.startsWith("/dev")) return false; // Devices
  if (target?.includes("<(")) return false; // Process substitution

  // Check if it looks like a regular file
  if (
    target?.match(
      /\.(js|ts|py|go|rs|cpp|c|h|json|yml|yaml|md|txt|sh|bash|zsh|html|css|scss|less|vue|jsx|tsx)$/i
    )
  ) {
    return true;
  }

  // If there's pipe from another command, it's a stream
  if (cmd.raw.includes("|")) return false;

  // Default: be conservative
  return true;
}

// ============================================================================
// ALWAYS ALLOWED COMMANDS
// ============================================================================

const ALWAYS_ALLOWED_COMMANDS = [
  // Git (except checkout/reset)
  "git:clone",
  "git:init",
  "git:add",
  "git:commit",
  "git:push",
  "git:pull",
  "git:status",
  "git:log",
  "git:diff",
  "git:branch",
  "git:remote",
  "git:config",
  "git:fetch",
  "git:merge",

  // Directory operations
  "mkdir",
  "mkdirp",
  "rmdir",
  "cd",
  "pwd",

  // File creation
  "touch",

  // File copy/move (be careful with paths)
  "cp",
  "mv",

  // Dependency managers
  "npm:install",
  "npm:ci",
  "npm:add",
  "yarn:add",
  "yarn:install",
  "pnpm:add",
  "pnpm:install",
  "pip:install",
  "pip3:install",
  "go:mod",
  "go:get",
  "cargo:install",
  "cargo:add",
  "cargo:build",
  "gem:install",
  "bundle:install",
  "composer:install",
  "composer:require",
  "mvn:install",
  "gradle:build",

  // Build tools
  "make",
  "cmake",
  "gcc",
  "g++",
  "clang",
  "rustc",
  "python",
  "python3",
  "node",
  "bun",
  "deno",

  // Archive tools
  "tar",
  "unzip",
  "zip",
  "gzip",
  "gunzip",

  // Network (careful)
  "curl",
  "wget",
  "git",

  // System info
  "uname",
  "which",
  "whereis",
  "type",
  "echo",
  "printf",
  "env",
  "export",
  "source",

  // Process
  "ps",
  "top",
  "htop",
  "kill",
  "pkill",
  "pgrep"
];

/**
 * Checks if a command is in the always-allowed list
 */
function isAlwaysAllowed(cmd: ParsedCommand): boolean {
  const cmdKey = cmd.subcommand ? `${cmd.command}:${cmd.subcommand}` : cmd.command;

  if (ALWAYS_ALLOWED_COMMANDS.includes(cmdKey)) return true;
  if (ALWAYS_ALLOWED_COMMANDS.includes(cmd.command)) return true;

  // Special case: git operations other than checkout/reset --hard
  if (cmd.command === "git") {
    const blockedSubcommands = ["checkout", "reset"];
    return !blockedSubcommands.includes(cmd.subcommand || "");
  }

  return false;
}

// ============================================================================
// MAIN ANALYSIS FUNCTION
// ============================================================================

/**
 * Analyzes a command and returns security assessment
 */
export function analyzeCommand(rawCmd: string): CommandAnalysis {
  const parsed = parseCommand(rawCmd);

  // First check: Is it always allowed?
  if (isAlwaysAllowed(parsed)) {
    // Even always-allowed commands can have CRITICAL issues (like shell injection)
    const criticalRule = SECURITY_RULES.find(
      (rule) => rule.severity === SecuritySeverity.CRITICAL && rule.check(parsed)
    );

    if (criticalRule) {
      return {
        allowed: false,
        severity: SecuritySeverity.CRITICAL,
        reason: criticalRule.reason,
        redirect: criticalRule.redirect,
        details: {
          parsedCommand: parsed,
          blockedOperations: parsed.detectedOperations,
          riskyPaths: parsed.paths.filter(
            (p) => p.type === PathType.SENSITIVE || p.type === PathType.SYSTEM
          )
        }
      };
    }

    return {
      allowed: true,
      severity: SecuritySeverity.ALLOWED,
      reason: `Command '${parsed.command}' is explicitly allowed`,
      details: {
        parsedCommand: parsed,
        blockedOperations: [],
        riskyPaths: []
      }
    };
  }

  // Check security rules
  for (const rule of SECURITY_RULES) {
    if (rule.check(parsed)) {
      return {
        allowed: rule.severity !== SecuritySeverity.CRITICAL && rule.severity !== SecuritySeverity.HIGH,
        severity: rule.severity,
        reason: rule.reason,
        redirect: rule.redirect,
        details: {
          parsedCommand: parsed,
          blockedOperations: parsed.detectedOperations,
          riskyPaths: parsed.paths.filter(
            (p) => p.type === PathType.SENSITIVE || p.type === PathType.SYSTEM
          )
        }
      };
    }
  }

  // Check for temp directory access (always fine)
  const allPathsAreTemp = parsed.paths.every((p) => p.type === PathType.TEMP);
  if (allPathsAreTemp && parsed.paths.length > 0) {
    return {
      allowed: true,
      severity: SecuritySeverity.ALLOWED,
      reason: "All paths are in temp directories",
      details: {
        parsedCommand: parsed,
        blockedOperations: [],
        riskyPaths: []
      }
    };
  }

  // Default: allow with low severity (unknown command)
  return {
    allowed: true,
    severity: SecuritySeverity.LOW,
    reason: `Unrecognized command '${parsed.command}' - use with caution`,
    details: {
      parsedCommand: parsed,
      blockedOperations: [],
      riskyPaths: parsed.paths.filter(
        (p) => p.type === PathType.SENSITIVE || p.type === PathType.SYSTEM
      )
    }
  };
}

/**
 * Quick check function for backward compatibility
 */
export function checkBlockedCommand(rawCmd: string): { blocked: boolean; redirect?: string } {
  const analysis = analyzeCommand(rawCmd);
  return {
    blocked: !analysis.allowed,
    redirect: analysis.redirect
  };
}
