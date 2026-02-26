import { logger } from "@firmius/shared/utils";

// =============================================================================
// CONSTANTS
// =============================================================================

const GO_WORKSPACE_INIT_TIMEOUT = 30000;
const LUAU_LSP_WARMUP_TIMEOUT = 20000;
const MAX_GO_MODULES = 20;
const ROJO_MANIFEST_FILE = "default.project.json";
const WALLY_MANIFEST_FILE = "wally.toml";
const AFTMAN_MANIFEST_FILE = "aftman.toml";
const ROKIT_MANIFEST_FILE = "rokit.toml";

const LOGGER_PREFIX = "[LSPUtility:LanguageHandler]";

// =============================================================================
// INTERFACES
// =============================================================================

/**
 * Interface for handler host operations
 */
export interface IHandlerHost {
  exec(command: string, options?: any): Promise<{ stdout: string; exitCode?: number }>;
  readFile(path: string): Promise<string>;
  writeFile(path: string, content: string): Promise<void>;
}

/**
 * Interface for handler registry operations
 */
export interface IHandlerRegistry {
  getClientForFile(filePath: string): Promise<any>;
  openDocument(filePath: string, content: string): Promise<void>;
  warmupLanguage(language: string): Promise<void>;
}

/**
 * Rojo manifest structure (default.project.json)
 */
export interface RojoManifest {
  name: string;
  tree: Record<string, any>;
}

/**
 * Luau workspace information
 */
export interface LuauWorkspaceInfo {
  rootPath: string;
  sourcePaths: string[];
  packagesPaths: string[];
}

/**
 * Result of finding Luau files
 */
export interface LuauFilesResult {
  luauFiles: string[];
  luaFiles: string[];
}

// =============================================================================
// LANGUAGE HANDLER CLASS
// =============================================================================

/**
 * Handles language-specific setup and configuration for LSP workspaces.
 * Includes Go workspace management and Luau/Roblox-specific setup.
 */
export class LanguageHandler {
  private host: IHandlerHost;
  private registry: IHandlerRegistry;
  private rootUri: string;

  constructor(host: IHandlerHost, registry: IHandlerRegistry, rootUri: string) {
    this.host = host;
    this.registry = registry;
    this.rootUri = rootUri;
  }

  // ===========================================================================
  // GO WORKSPACE METHODS
  // ===========================================================================

  /**
   * Sets up Go workspaces for multi-module projects.
   * Creates go.work file if multiple go.mod files are found.
   */
  async setupGoWorkspaces(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Checking for Go workspace setup...`);

    try {
      const isMultiModule = await this.isMultiModuleGoProject();

      if (!isMultiModule) {
        logger.debug(`${LOGGER_PREFIX} Single Go module project, no go.work needed`);
        return;
      }

      const goWorkExists = await this.goWorkExists();

      if (goWorkExists) {
        logger.info(`${LOGGER_PREFIX} go.work already exists, skipping creation`);
        return;
      }

      await this.createGoWorkFile();
      await this.downloadGoDependencies();
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to setup Go workspace: ${error}`);
    }
  }

  /**
   * Checks if this is a multi-module Go project.
   */
  async isMultiModuleGoProject(): Promise<boolean> {
    try {
      const goModResult = await this.host.exec(
        `find . -name "go.mod" -not -path "./vendor/*" 2>/dev/null | head -${MAX_GO_MODULES}`,
        { cwd: this.rootUri, timeout: GO_WORKSPACE_INIT_TIMEOUT }
      );

      const goModFiles = goModResult.stdout.trim().split("\n").filter((f) => f);

      return goModFiles.length > 1;
    } catch (error) {
      logger.debug(`${LOGGER_PREFIX} Error checking for Go modules: ${error}`);
      return false;
    }
  }

  /**
   * Checks if go.work file exists.
   */
  private async goWorkExists(): Promise<boolean> {
    try {
      const result = await this.host.exec(`test -f go.work && echo "exists" || echo "missing"`, {
        cwd: this.rootUri,
      });
      return result.stdout.trim() === "exists";
    } catch (error) {
      return false;
    }
  }

  /**
   * Creates go.work file for multi-module workspace.
   */
  private async createGoWorkFile(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Creating go.work for multi-module Go workspace...`);

    try {
      // Find all go.mod files
      const goModResult = await this.host.exec(
        `find . -name "go.mod" -not -path "./vendor/*" 2>/dev/null | head -${MAX_GO_MODULES}`,
        { cwd: this.rootUri }
      );

      const goModFiles = goModResult.stdout.trim().split("\n").filter((f) => f);

      // Initialize go.work
      await this.host.exec(`GOTOOLCHAIN=auto go work init`, {
        cwd: this.rootUri,
        timeout: GO_WORKSPACE_INIT_TIMEOUT,
      });

      // Add each module to the workspace
      for (const modFile of goModFiles) {
        const modDir = modFile.replace("/go.mod", "").replace("./", "");
        if (modDir) {
          const modulePath = modDir === "." ? "." : `./${modDir}`;
          try {
            await this.host.exec(`GOTOOLCHAIN=auto go work use ${modulePath}`, {
              cwd: this.rootUri,
              timeout: GO_WORKSPACE_INIT_TIMEOUT,
            });
            logger.debug(`${LOGGER_PREFIX} Added ${modulePath} to go.work`);
          } catch (e) {
            logger.debug(`${LOGGER_PREFIX} Failed to add ${modulePath} to go.work: ${e}`);
          }
        }
      }

      logger.info(`${LOGGER_PREFIX} Created go.work with ${goModFiles.length} modules`);
    } catch (error) {
      throw new Error(`Failed to create go.work: ${error}`);
    }
  }

  /**
   * Downloads Go module dependencies.
   */
  private async downloadGoDependencies(): Promise<void> {
    try {
      const goModResult = await this.host.exec(
        `find . -name "go.mod" -not -path "./vendor/*" 2>/dev/null | head -${MAX_GO_MODULES}`,
        { cwd: this.rootUri }
      );

      const goModFiles = goModResult.stdout.trim().split("\n").filter((f) => f);

      logger.info(`${LOGGER_PREFIX} Downloading Go dependencies for ${goModFiles.length} modules...`);

      for (const modFile of goModFiles) {
        const modDir = modFile.replace("/go.mod", "").replace("./", "");
        if (modDir) {
          try {
            await this.host.exec(`GOTOOLCHAIN=auto go mod download`, {
              cwd: `${this.rootUri}/${modDir}`,
              timeout: 120000,
            });
            logger.debug(`${LOGGER_PREFIX} Downloaded dependencies for ${modDir}`);
          } catch (e) {
            logger.debug(`${LOGGER_PREFIX} go mod download failed for ${modDir}: ${e}`);
          }
        }
      }

      logger.info(`${LOGGER_PREFIX} Downloaded Go modules for workspace`);
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to download Go dependencies: ${error}`);
    }
  }

  // ===========================================================================
  // LUAU WORKSPACE METHODS
  // ===========================================================================

  /**
   * Sets up Luau workspace with Rojo manifest parsing and file discovery.
   * @returns LuauWorkspaceInfo or null if not a Luau project
   */
  async setupLuauWorkspace(): Promise<LuauWorkspaceInfo | null> {
    logger.info(`${LOGGER_PREFIX} Setting up Luau workspace...`);

    try {
      // Check for Rojo manifest
      const rojoManifest = await this.parseRojoManifest();

      if (!rojoManifest) {
        logger.debug(`${LOGGER_PREFIX} No Rojo manifest found, checking for Luau files...`);
      } else {
        logger.info(`${LOGGER_PREFIX} Found Rojo project: ${rojoManifest.name}`);
      }

      // Find all Luau files
      const files = await this.findLuauFiles();

      if (files.luauFiles.length === 0 && files.luaFiles.length === 0) {
        logger.info(`${LOGGER_PREFIX} No Luau/Lua files found`);
        return null;
      }

      logger.info(
        `${LOGGER_PREFIX} Found ${files.luauFiles.length} .luau files and ${files.luaFiles.length} .lua files`
      );

      // Extract source paths from Rojo manifest if available
      const sourcePaths: string[] = [];
      if (rojoManifest?.tree) {
        const extractedPaths = this.extractRojoSourcePaths(rojoManifest.tree);
        sourcePaths.push(...extractedPaths);
      }

      // Find Packages directories (important for Luau dependency management)
      const packagesPaths = await this.findPackagesDirectories();

      const workspaceInfo: LuauWorkspaceInfo = {
        rootPath: this.rootUri,
        sourcePaths: sourcePaths.length > 0 ? sourcePaths : [this.rootUri],
        packagesPaths,
      };

      logger.info(`${LOGGER_PREFIX} Luau workspace setup complete`);
      return workspaceInfo;
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to setup Luau workspace: ${error}`);
      return null;
    }
  }

  /**
   * Parses the Rojo manifest (default.project.json) for Luau projects.
   * @returns RojoManifest or null if not found
   */
  async parseRojoManifest(): Promise<RojoManifest | null> {
    const manifestPath = `${this.rootUri}/${ROJO_MANIFEST_FILE}`;

    try {
      const content = await this.host.readFile(manifestPath);
      const parsed = JSON.parse(content) as RojoManifest;

      if (!parsed.name || !parsed.tree) {
        logger.warn(`${LOGGER_PREFIX} Invalid Rojo manifest: missing name or tree`);
        return null;
      }

      logger.info(`${LOGGER_PREFIX} Parsed Rojo manifest for project: ${parsed.name}`);
      return parsed;
    } catch (error) {
      logger.debug(`${LOGGER_PREFIX} No Rojo manifest found at ${manifestPath}`);
      return null;
    }
  }

  /**
   * Finds all Luau and Lua files in the workspace.
   */
  async findLuauFiles(): Promise<LuauFilesResult> {
    try {
      // Find .luau files
      const luauResult = await this.host.exec(
        `find . -name "*.luau" -type f 2>/dev/null | head -100`,
        { cwd: this.rootUri }
      );
      const luauFiles = luauResult.stdout.trim().split("\n").filter((f) => f);

      // Find .lua files
      const luaResult = await this.host.exec(
        `find . -name "*.lua" -type f 2>/dev/null | head -100`,
        { cwd: this.rootUri }
      );
      const luaFiles = luaResult.stdout.trim().split("\n").filter((f) => f);

      return {
        luauFiles,
        luaFiles,
      };
    } catch (error) {
      logger.debug(`${LOGGER_PREFIX} Error finding Luau files: ${error}`);
      return { luauFiles: [], luaFiles: [] };
    }
  }

  /**
   * Extracts source paths from Rojo manifest tree structure.
   */
  private extractRojoSourcePaths(tree: Record<string, any>, currentPath: string = ""): string[] {
    const paths: string[] = [];

    for (const [key, value] of Object.entries(tree)) {
      if (typeof value === "object" && value !== null) {
        // Check for $path entries
        if (value.$path) {
          const fullPath = currentPath ? `${currentPath}/${value.$path}` : value.$path;
          paths.push(fullPath);
        }

        // Recursively process nested tree entries
        if (key === "$className" || key === "$path" || key === "$ignoreUnknownInstances") {
          continue;
        }

        const childPaths = this.extractRojoSourcePaths(value, currentPath ? `${currentPath}/${key}` : key);
        paths.push(...childPaths);
      }
    }

    return paths;
  }

  /**
   * Finds Packages directories (Luau package manager directories).
   */
  private async findPackagesDirectories(): Promise<string[]> {
    try {
      const result = await this.host.exec(
        `find . -type d -name "Packages" 2>/dev/null`,
        { cwd: this.rootUri }
      );

      return result.stdout.trim().split("\n").filter((d) => d);
    } catch (error) {
      logger.debug(`${LOGGER_PREFIX} Error finding Packages directories: ${error}`);
      return [];
    }
  }

  // ===========================================================================
  // LANGUAGE PREWARMING METHODS
  // ===========================================================================

  /**
   * Prewarms LSP for a specific language.
   * Opens key files to trigger workspace indexing.
   */
  async prewarmLanguage(language: string): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Prewarming LSP for language: ${language}`);

    try {
      switch (language.toLowerCase()) {
        case "go":
          await this.prewarmGoLSP();
          break;
        case "luau":
          await this.prewarmLuauLSP();
          break;
        case "typescript":
        case "ts":
          await this.prewarmTypeScriptLSP();
          break;
        case "python":
        case "py":
          await this.prewarmPythonLSP();
          break;
        default:
          logger.debug(`${LOGGER_PREFIX} No specific prewarming for language: ${language}`);
      }
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to prewarm ${language} LSP: ${error}`);
    }
  }

  /**
   * Specifically prewarms luau-lsp.
   * Opens key Luau files and waits for workspace initialization.
   */
  async prewarmLuauLSP(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Prewarming luau-lsp...`);

    try {
      // Get luau-lsp client from registry
      const client = await this.registry.getClientForFile(`${this.rootUri}/dummy.luau`);

      if (!client) {
        logger.warn(`${LOGGER_PREFIX} luau-lsp client not available`);
        return;
      }

      // Find key Luau files to open
      const files = await this.findLuauFiles();
      const keyFiles = this.identifyKeyLuauFiles([...files.luauFiles, ...files.luaFiles]);

      logger.info(`${LOGGER_PREFIX} Opening ${keyFiles.length} key Luau files for prewarming...`);

      // Open each key file
      for (const file of keyFiles.slice(0, 5)) {
        try {
          const content = await this.host.readFile(`${this.rootUri}/${file}`);
          await this.registry.openDocument(`${this.rootUri}/${file}`, content);
          logger.debug(`${LOGGER_PREFIX} Opened ${file} for prewarming`);
        } catch (e) {
          logger.debug(`${LOGGER_PREFIX} Failed to open ${file}: ${e}`);
        }
      }

      // Wait for workspace/didChangeWatchedFiles notification (if supported)
      await new Promise((resolve) => setTimeout(resolve, 2000));

      logger.info(`${LOGGER_PREFIX} luau-lsp prewarming complete`);
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to prewarm luau-lsp: ${error}`);
    }
  }

  /**
   * Identifies key Luau files for prewarming.
   * Prioritizes entry points and important modules.
   */
  private identifyKeyLuauFiles(files: string[]): string[] {
    const priorityPatterns = [
      /init\.luau$/,
      /init\.lua$/,
      /main\.luau$/,
      /main\.lua$/,
      /server\.luau$/,
      /server\.lua$/,
      /client\.luau$/,
      /client\.lua$/,
      /index\.luau$/,
      /index\.lua$/,
    ];

    // Sort files by priority
    const sorted = [...files].sort((a, b) => {
      const aPriority = priorityPatterns.findIndex((p) => p.test(a));
      const bPriority = priorityPatterns.findIndex((p) => p.test(b));

      if (aPriority === -1 && bPriority === -1) return 0;
      if (aPriority === -1) return 1;
      if (bPriority === -1) return -1;

      return aPriority - bPriority;
    });

    return sorted;
  }

  /**
   * Prewarms Go LSP (gopls).
   */
  private async prewarmGoLSP(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Prewarming gopls...`);

    try {
      const result = await this.host.exec(
        `find . -name "*.go" -type f 2>/dev/null | head -10`,
        { cwd: this.rootUri }
      );

      const goFiles = result.stdout.trim().split("\n").filter((f) => f);

      if (goFiles.length === 0) {
        logger.debug(`${LOGGER_PREFIX} No Go files found for prewarming`);
        return;
      }

      const client = await this.registry.getClientForFile(`${this.rootUri}/dummy.go`);

      if (!client) {
        logger.warn(`${LOGGER_PREFIX} gopls client not available`);
        return;
      }

      // Open first few files to trigger indexing
      for (const file of goFiles.slice(0, 3)) {
        try {
          const content = await this.host.readFile(`${this.rootUri}/${file}`);
          await this.registry.openDocument(`${this.rootUri}/${file}`, content);
        } catch (e) {
          logger.debug(`${LOGGER_PREFIX} Failed to open ${file}: ${e}`);
        }
      }

      logger.info(`${LOGGER_PREFIX} gopls prewarming complete`);
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to prewarm gopls: ${error}`);
    }
  }

  /**
   * Prewarms TypeScript LSP.
   */
  private async prewarmTypeScriptLSP(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Prewarming TypeScript LSP...`);

    try {
      const result = await this.host.exec(
        `find . -name "*.ts" -type f ! -path "*/node_modules/*" 2>/dev/null | head -5`,
        { cwd: this.rootUri }
      );

      const tsFiles = result.stdout.trim().split("\n").filter((f) => f);

      if (tsFiles.length === 0) {
        logger.debug(`${LOGGER_PREFIX} No TypeScript files found for prewarming`);
        return;
      }

      const client = await this.registry.getClientForFile(`${this.rootUri}/dummy.ts`);

      if (!client) {
        logger.warn(`${LOGGER_PREFIX} TypeScript LSP client not available`);
        return;
      }

      // Open first few files
      for (const file of tsFiles.slice(0, 3)) {
        try {
          const content = await this.host.readFile(`${this.rootUri}/${file}`);
          await this.registry.openDocument(`${this.rootUri}/${file}`, content);
        } catch (e) {
          logger.debug(`${LOGGER_PREFIX} Failed to open ${file}: ${e}`);
        }
      }

      logger.info(`${LOGGER_PREFIX} TypeScript LSP prewarming complete`);
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to prewarm TypeScript LSP: ${error}`);
    }
  }

  /**
   * Prewarms Python LSP.
   */
  private async prewarmPythonLSP(): Promise<void> {
    logger.info(`${LOGGER_PREFIX} Prewarming Python LSP...`);

    try {
      const result = await this.host.exec(
        `find . -name "*.py" -type f 2>/dev/null | head -5`,
        { cwd: this.rootUri }
      );

      const pyFiles = result.stdout.trim().split("\n").filter((f) => f);

      if (pyFiles.length === 0) {
        logger.debug(`${LOGGER_PREFIX} No Python files found for prewarming`);
        return;
      }

      const client = await this.registry.getClientForFile(`${this.rootUri}/dummy.py`);

      if (!client) {
        logger.warn(`${LOGGER_PREFIX} Python LSP client not available`);
        return;
      }

      // Open first few files
      for (const file of pyFiles.slice(0, 3)) {
        try {
          const content = await this.host.readFile(`${this.rootUri}/${file}`);
          await this.registry.openDocument(`${this.rootUri}/${file}`, content);
        } catch (e) {
          logger.debug(`${LOGGER_PREFIX} Failed to open ${file}: ${e}`);
        }
      }

      logger.info(`${LOGGER_PREFIX} Python LSP prewarming complete`);
    } catch (error) {
      logger.warn(`${LOGGER_PREFIX} Failed to prewarm Python LSP: ${error}`);
    }
  }
}

// Export constants for external use
export {
  GO_WORKSPACE_INIT_TIMEOUT,
  LUAU_LSP_WARMUP_TIMEOUT,
  MAX_GO_MODULES,
  ROJO_MANIFEST_FILE,
  WALLY_MANIFEST_FILE,
  AFTMAN_MANIFEST_FILE,
  ROKIT_MANIFEST_FILE,
  LOGGER_PREFIX,
};
