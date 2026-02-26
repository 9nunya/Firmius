#!/usr/bin/env bun

import { tmpdir } from "node:os";
import { join } from "node:path";
import { mkdir, writeFile, rm } from "node:fs/promises";
import { existsSync } from "node:fs";

import { purposeRegistry } from "@firmius/core";
import { createServer } from "@firmius/api/server";
import { EMBEDDED_PURPOSES } from "./purposes.embedded";

// Set embedded purposes globally before any registry init
globalThis.EMBEDDED_PURPOSES = EMBEDDED_PURPOSES;

let extractedWebDir: string | null = null;

async function extractWebStatic(): Promise<string> {
  if (extractedWebDir) return extractedWebDir;

  const webStatic = await import("./web.embedded");
  const base64Data = webStatic.EMBEDDED_WEB_STATIC;

  const tmpDir = join(tmpdir(), `firmius-web-${Date.now()}`);
  await mkdir(tmpDir, { recursive: true });

  const tarPath = join(tmpDir, "web.tar.gz");
  const tarBuffer = Buffer.from(base64Data, "base64");
  await writeFile(tarPath, tarBuffer);

  const { spawn } = await import("node:child_process");
  await new Promise<void>((resolve, reject) => {
    const child = spawn("tar", ["-xzf", "web.tar.gz"], { cwd: tmpDir, stdio: "inherit" });
    (child as any).on("close", (code: number | null) => (code === 0 ? resolve() : reject(new Error(`tar failed: ${code}`))));
  });

  await rm(tarPath, { force: true });

  extractedWebDir = tmpDir;
  return tmpDir;
}

const args = Bun.argv.slice(2);
const command = args[0];

interface Options {
  port: number;
  host: string;
  quiet: boolean;
  httpLog: boolean;
  prettyPrint: boolean;
}

function parseOptions(args: string[]): Options {
  const options: Options = {
    port: 9817,
    host: "0.0.0.0",
    quiet: false,
    httpLog: false,
    prettyPrint: false,
  };

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    if (arg === "--port" && args[i + 1]) {
      options.port = parseInt(args[i + 1]!, 10);
      i++;
    } else if (arg === "--host" && args[i + 1]) {
      options.host = args[i + 1]!;
      i++;
    } else if (arg === "--quiet" || arg === "-q") {
      options.quiet = true;
    } else if (arg === "--http-log") {
      options.httpLog = true;
    } else if (arg === "--pretty-print") {
      options.prettyPrint = true;
    } else if (arg === "--verbose" || arg === "-v") {
      options.quiet = true;
      options.httpLog = true;
      options.prettyPrint = true;
    }
  }

  return options;
}

async function createStaticFileHandler(): Promise<(pathname: string) => Promise<Response | null>> {
  const webDir = await extractWebStatic();
  
  return async (pathname: string): Promise<Response | null> => {
    // Next.js static files
    if (pathname.startsWith("/_next/")) {
      const filePath = join(webDir, pathname);
      try {
        const file = Bun.file(filePath);
        if (await file.exists()) {
          return new Response(file);
        }
      } catch {
        // File doesn't exist
      }
    }

    // Root/index.html
    if (pathname === "/" || pathname === "") {
      const indexPath = join(webDir, "index.html");
      try {
        const file = Bun.file(indexPath);
        if (await file.exists()) {
          return new Response(file);
        }
      } catch {
        // File doesn't exist
      }
    }

    // Try to serve as static file
    let filePath = join(webDir, pathname);
    if (!filePath.endsWith("/") && existsSync(join(filePath, "index.html"))) {
      filePath = join(filePath, "index.html");
    }
    
    try {
      const file = Bun.file(filePath);
      if (await file.exists()) {
        return new Response(file);
      }
    } catch {
      // File doesn't exist
    }

    return null;
  };
}

async function runServer(options: Options): Promise<void> {
  await purposeRegistry.init();
  if (!options.quiet) {
    console.log("✅ Purposes loaded");
  }

  const staticFileHandler = await createStaticFileHandler();
  
  if (!options.quiet) {
    console.log("📦 Web static ready");
  }

  const { shutdown } = await createServer({
    port: options.port,
    hostname: options.host,
    quiet: options.quiet,
    httpLog: options.httpLog,
    prettyPrint: options.prettyPrint,
    staticFileHandler,
  });

  const shutdownHandler = async (signal: string): Promise<void> => {
    if (!options.quiet) {
      console.log(`\n⚠️  Received ${signal} signal`);
    }
    await shutdown();
    process.exit(0);
  };

  process.on("SIGTERM", () => shutdownHandler("SIGTERM"));
  process.on("SIGINT", () => shutdownHandler("SIGINT"));

  console.log(`🚀 Firmius running on http://${options.host}:${options.port}`);
}

async function runTui(): Promise<void> {
  console.log("Starting TUI...");
  const { createCliRenderer } = await import("@opentui/core");
  const { createRoot } = await import("@opentui/react");
  
  const renderer = await createCliRenderer();
  const root = createRoot(renderer);

  const tuiModule = await import("@firmius/tui");
  const App = tuiModule.App;
  root.render(<App />);

  process.on("SIGINT", () => {
    renderer.destroy();
    process.exit(0);
  });
  process.on("SIGTERM", () => {
    renderer.destroy();
    process.exit(0);
  });
}

switch (command) {
  case "tui":
    await runTui();
    break;
  case "server": {
    const options = parseOptions(args.slice(1));
    await runServer(options);
    break;
  }
  case "start": {
    const options = parseOptions(args.slice(1));
    await runServer(options);
    break;
  }
  case "help":
  case "--help":
  case "-h":
  default:
    if (command && command !== "help" && command !== "--help" && command !== "-h") {
      console.log(`Unknown command: ${command}\n`);
    }
    console.log(`Usage: firmius <command> [options]

Commands:
  server         Start the unified server (API + Web UI)
  start          Alias for server
  tui            Run terminal UI
  help           Show this help message

Options:
  --port N           Port for server (default: 9817)
  --host IP          Host to bind to (default: 0.0.0.0)
  --quiet, -q        Suppress startup output
  --http-log         Enable HTTP request/response logging
  --pretty-print     Enable agent output pretty-printing
  --verbose, -v      Enable all logging (equivalent to --http-log --pretty-print)`);
}
