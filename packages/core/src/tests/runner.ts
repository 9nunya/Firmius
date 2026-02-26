#!/usr/bin/env bun
import { createConnection } from "node:net";
import path from "node:path";

const projectBin = path.resolve(process.cwd(), "node_modules", ".bin");
process.env.PATH = `${projectBin}:${process.env.PATH}`;

const SSH_CONTAINER_NAME = "firmius-ssh-test-srv";

async function isPortOpen(port: number): Promise<boolean> {
  return new Promise((resolve) => {
    const socket = createConnection(port, "localhost");
    socket.on("connect", () => {
      socket.destroy();
      resolve(true);
    });
    socket.on("error", () => {
      resolve(false);
    });
  });
}

async function startSSHServer(): Promise<void> {
  Bun.spawnSync(["docker", "rm", "-f", SSH_CONTAINER_NAME]);
  const run = Bun.spawn([
    "docker",
    "run",
    "-d",
    "--name",
    SSH_CONTAINER_NAME,
    "-p",
    "2222:22",
    "firmius-sandbox:latest",
  ]);
  await run.exited;

  let attempts = 0;
  while (attempts < 20) {
    if (await isPortOpen(2222)) {
      await new Promise((r) => setTimeout(r, 2000));
      break;
    }
    await new Promise((r) => setTimeout(r, 1000));
    attempts++;
  }

  if (attempts === 20) {
    throw new Error("SSH test server failed to start in time");
  }
}

async function stopSSHServer(): Promise<void> {
  Bun.spawnSync(["docker", "rm", "-f", SSH_CONTAINER_NAME]);
}

type HostType = "local" | "docker" | "ssh" | "all";
type SuiteName =
  | "file"
  | "process"
  | "lsp"
  | "delegation"
  | "context"
  | "web"
  | "todo"
  | "code"
  | "subagent"
  | "all";

function parseArgs(): { hosts: HostType[]; suites: SuiteName[] } {
  const args = process.argv.slice(2);
  let hosts: HostType[] = ["local"];
  let suites: SuiteName[] = ["all"];

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];

    if (arg === "--hosts" || arg === "-H") {
      const hostListArg = args[++i];
      if (hostListArg) {
        const hostList = hostListArg.toLowerCase().split(",");
        if (hostList.includes("all")) {
          hosts = ["local", "docker", "ssh"];
        } else {
          hosts = hostList.filter((h): h is HostType =>
            ["local", "docker", "ssh"].includes(h),
          );
        }
      }
    } else if (arg === "--suites" || arg === "-S") {
      const suiteListArg = args[++i];
      if (suiteListArg) {
        const suiteList = suiteListArg.toLowerCase().split(",");
        if (suiteList.includes("all")) {
          suites = ["all"];
        } else {
          suites = suiteList.filter((s): s is SuiteName =>
            [
              "file",
              "process",
              "lsp",
              "delegation",
              "context",
              "web",
              "todo",
              "code",
              "subagent",
            ].includes(s),
          );
        }
      }
    } else if (arg === "--help" || arg === "-h") {
      printHelp();
      process.exit(0);
    }
  }

  return { hosts, suites };
}

function printHelp() {
  console.log(`
Test Runner - Select hosts and suites to run

Usage:
  bun tests/runner.ts [options]

Options:
  -H, --hosts <hosts>    Comma-separated list of hosts: local,docker,ssh,all
                          Default: local

  -S, --suites <suites>  Comma-separated list of suites: file,process,lsp,delegation,context,web,todo,code,subagent,all
                          Default: all

  -h, --help              Show this help message

Examples:
  bun tests/runner.ts                          # Run all suites on local host
  bun tests/runner.ts -H docker              # Run all suites on docker host
  bun tests/runner.ts -H local,docker -S lsp,delegation
  bun tests/runner.ts -H all -S all            # Run everything on all hosts
  `);
}

async function runTests() {
  const { hosts, suites } = parseArgs();

  const activeHosts = hosts.filter(
    (h): h is "local" | "docker" | "ssh" => h !== "all",
  );
  const activeSuites = suites.includes("all")
    ? (["all"] as SuiteName[])
    : suites;

  const needsSSH = activeHosts.includes("ssh");
  if (needsSSH) {
    console.log("[SSH Test] Starting SSH test server using firmius-sandbox...");
    await startSSHServer();
    console.log("[SSH Test] SSH test server ready.");
  }

  try {
    console.log(`\n🧪 Running tests on hosts: ${activeHosts.join(", ")}`);
    console.log(`🧪 Running suites: ${activeSuites.join(", ")}\n`);

    const testFiles: string[] = [];

    if (activeHosts.includes("local")) {
      if (
        activeSuites.includes("all") ||
        activeSuites.some((s) =>
          [
            "file",
            "process",
            "lsp",
            "delegation",
            "context",
            "web",
            "todo",
            "code",
            "subagent",
          ].includes(s),
        )
      ) {
        testFiles.push("tests/integration/LocalTools.test.ts");
      }
    }
    if (activeHosts.includes("docker")) {
      if (
        activeSuites.includes("all") ||
        activeSuites.some((s) =>
          [
            "file",
            "process",
            "lsp",
            "delegation",
            "context",
            "web",
            "todo",
            "code",
            "subagent",
          ].includes(s),
        )
      ) {
        testFiles.push("tests/integration/DockerTools.test.ts");
      }
    }
    if (activeHosts.includes("ssh")) {
      if (
        activeSuites.includes("all") ||
        activeSuites.some((s) =>
          [
            "file",
            "process",
            "lsp",
            "delegation",
            "context",
            "web",
            "todo",
            "code",
            "subagent",
          ].includes(s),
        )
      ) {
        testFiles.push("tests/integration/RemoteSSHTools.test.ts");
      }
    }

    if (testFiles.length === 0) {
      console.log("No tests to run based on selection.");
      return;
    }

    const suiteFilter = suites.includes("all") ? "all" : suites.join(",");
    const env = { ...process.env, FIRMUS_TEST_SUITES: suiteFilter };
    const proc = Bun.spawn(["bun", "test", ...testFiles], { env });
    const exitCode = await proc.exited;

    console.log(`\n✨ Test run complete!\n`);

    if (exitCode !== 0) {
      process.exit(exitCode);
    }
  } finally {
    if (needsSSH) {
      console.log("[SSH Test] Cleaning up SSH test server...");
      await stopSSHServer();
    }
  }
}

runTests().catch(console.error);
