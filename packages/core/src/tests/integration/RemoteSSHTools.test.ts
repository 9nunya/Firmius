import { beforeAll, afterAll } from "bun:test";
import { runFileToolsTests } from "../suites/FileTools.suite";
import { runProcessToolsTests } from "../suites/ProcessTools.suite";
import { runLSPToolsTests } from "../suites/LSPTools.suite";
import { runDelegationToolsTests } from "../suites/DelegationTools.suite";
import { runContextToolsTests } from "../suites/ContextTools.suite";
import { runWebToolsTests } from "../suites/WebTools.suite";
import { runTodoToolsTests } from "../suites/TodoTools.suite";
import { runCodeToolsTests } from "../suites/CodeTools.suite";
import { runSubagentToolsTests } from "../suites/SubagentTools.suite";
import { RemoteSSHHost } from "../../hosts/RemoteSSHHost";
import { createConnection } from "node:net";

const CONTAINER_NAME = "firmius-ssh-test-srv";

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

beforeAll(async () => {
  console.log("[SSH Test] Starting SSH test server using firmius-sandbox...");

  // Remove if exists
  Bun.spawnSync(["docker", "rm", "-f", CONTAINER_NAME]);

  // Start firmius-sandbox with SSH server
  const run = Bun.spawn([
    "docker",
    "run",
    "-d",
    "--name",
    CONTAINER_NAME,
    "-p",
    "2222:22",
    "firmius-sandbox:latest",
  ]);
  await run.exited;

  // Wait for SSH to be ready
  let attempts = 0;
  while (attempts < 20) {
    if (await isPortOpen(2222)) {
      // Give it a bit more time for the service to actually be ready to handle requests
      await new Promise((r) => setTimeout(r, 2000));
      break;
    }
    await new Promise((r) => setTimeout(r, 1000));
    attempts++;
  }

  if (attempts === 20) {
    throw new Error("SSH test server failed to start in time");
  }

  console.log("[SSH Test] SSH test server ready.");
});

afterAll(async () => {
  console.log("[SSH Test] Cleaning up SSH test server...");
  Bun.spawnSync(["docker", "rm", "-f", CONTAINER_NAME]);
});

// The SSH integration test requires a running SSH server.
const getSSHHost = async () =>
  new RemoteSSHHost(
    {
      host: "localhost",
      port: 2222,
      username: "testuser",
      password: "testpassword",
      readyTimeout: 20000,
    },
    "/home/testuser",
  );

runFileToolsTests("RemoteSSHHost", getSSHHost);
runProcessToolsTests("RemoteSSHHost", getSSHHost);
runLSPToolsTests("RemoteSSHHost", getSSHHost);
runDelegationToolsTests("RemoteSSHHost", getSSHHost);
runContextToolsTests("RemoteSSHHost", getSSHHost);
runWebToolsTests("RemoteSSHHost", getSSHHost);
runTodoToolsTests("RemoteSSHHost", getSSHHost);
runCodeToolsTests("RemoteSSHHost", getSSHHost);
runSubagentToolsTests("RemoteSSHHost", getSSHHost);
