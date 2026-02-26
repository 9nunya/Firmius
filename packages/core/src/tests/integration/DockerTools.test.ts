import { runFileToolsTests } from "../suites/FileTools.suite";
import { runProcessToolsTests } from "../suites/ProcessTools.suite";
import { runLSPToolsTests } from "../suites/LSPTools.suite";
import { runDelegationToolsTests } from "../suites/DelegationTools.suite";
import { runContextToolsTests } from "../suites/ContextTools.suite";
import { runWebToolsTests } from "../suites/WebTools.suite";
import { runTodoToolsTests } from "../suites/TodoTools.suite";
import { runCodeToolsTests } from "../suites/CodeTools.suite";
import { runSubagentToolsTests } from "../suites/SubagentTools.suite";
import { DockerHost } from "../../hosts/DockerHost";

// Factory
const getDockerHost = async () =>
  new DockerHost({
    image: "firmius-sandbox:latest",
    containerName: `firmius-test-${Math.random().toString(36).substring(7)}`,
  });

runFileToolsTests("DockerHost", getDockerHost);
runProcessToolsTests("DockerHost", getDockerHost);
runLSPToolsTests("DockerHost", getDockerHost);
runDelegationToolsTests("DockerHost", getDockerHost);
runContextToolsTests("DockerHost", getDockerHost);
runWebToolsTests("DockerHost", getDockerHost);
runTodoToolsTests("DockerHost", getDockerHost);
runCodeToolsTests("DockerHost", getDockerHost);
runSubagentToolsTests("DockerHost", getDockerHost);
