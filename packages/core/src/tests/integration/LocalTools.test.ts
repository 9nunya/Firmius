import { runFileToolsTests } from "../suites/FileTools.suite";
import { runProcessToolsTests } from "../suites/ProcessTools.suite";
import { runLSPToolsTests } from "../suites/LSPTools.suite";
import { runDelegationToolsTests } from "../suites/DelegationTools.suite";
import { runContextToolsTests } from "../suites/ContextTools.suite";
import { runWebToolsTests } from "../suites/WebTools.suite";
import { runTodoToolsTests } from "../suites/TodoTools.suite";
import { runCodeToolsTests } from "../suites/CodeTools.suite";
import { runSubagentToolsTests } from "../suites/SubagentTools.suite";
import { LocalHost } from "../../hosts/Local";
import path from "node:path";

// Support for local LSP testing (assuming binaries in node_modules/.bin)
const projectBin = path.resolve(process.cwd(), "node_modules", ".bin");
process.env.PATH = `${projectBin}:${process.env.PATH}`;

// Factory
const getLocalHost = async () => new LocalHost();

runFileToolsTests("LocalHost", getLocalHost);
runProcessToolsTests("LocalHost", getLocalHost);
runLSPToolsTests("LocalHost", getLocalHost);
runDelegationToolsTests("LocalHost", getLocalHost);
runContextToolsTests("LocalHost", getLocalHost);
runWebToolsTests("LocalHost", getLocalHost);
runTodoToolsTests("LocalHost", getLocalHost);
runCodeToolsTests("LocalHost", getLocalHost);
runSubagentToolsTests("LocalHost", getLocalHost);
