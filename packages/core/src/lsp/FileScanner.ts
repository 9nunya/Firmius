import { logger } from "@firmius/shared/utils";

// ============================================================================
// Constants
// ============================================================================

const MAX_FILES_PER_LANGUAGE = 600;

const ENTRY_POINT_FILES = [
  "main",
  "index",
  "app",
];

const ROBLOX_ENTRY_PATTERNS = [
  "main.server.luau",
  "main.server.lua",
  "main.client.luau",
  "main.client.lua",
];

const SKIP_DIRS = [
  "node_modules",
  ".git",
  ".cache",
  "dist",
  "build",
  "out",
  "target",
  "__pycache__",
  ".tox",
  ".venv",
  "venv",
  ".idea",
  ".vscode",
  "bin",
  "obj",
  "third_party",
  "thirdparty",
  "3rdparty",
  "3rd_party",
  "vendor",
  "vendors",
  "external",
  "extern",
  "externals",
  "deps",
  "dependencies",
  "_deps",
  ".submodules",
  "submodules",
  ".nox",
  ".mypy_cache",
  "coverage",
  ".pytest_cache",
  "gen",
  "generated",
  "protobuf",
  "proto",
  "libs"
];

// Luau-specific: don't skip Packages directory
const LUAU_SKIP_EXCEPTIONS = ["packages"];

const LOW_PRIORITY_DIRS = [
  "test",
  "tests",
  "testing",
  "__tests__",
  "spec",
  "specs",
  "example",
  "examples",
  "samples",
  "sample",
  "demo",
  "demos",
  "docs",
  "documentation",
  "doc",
  "scripts",
  "script",
  "fixtures",
  "mocks",
  "stubs",
  "testdata",
  "test_data",
  "internal",
  "private",
];

const SOURCE_DIRS = ["src", "lib", "source"];

const LOGGER_PREFIX = "[LSPUtility:FileScanner]";

// ============================================================================
// Interfaces
// ============================================================================

export interface IFileScannerHost {
  readFile(path: string): Promise<string>;
  exec(command: string, options?: any): Promise<{ stdout: string }>;
  findFilesByExtension(ext: string, limit?: number): Promise<string[]>;
}

export interface IFileScannerRegistry {
  getAvailableLanguageExtensions(): string[];
}

export interface EntryPoint {
  name: string;
  path: string;
  type: "main" | "export" | "tool" | "roblox";
}

export interface FileStats {
  totalFiles: number;
  languages: string[];
  languageCounts: Record<string, number>;
  entryPoints: EntryPoint[];
}

interface RojoManifest {
  name: string;
  tree: Record<string, any>;
}

// ============================================================================
// FileScanner Class
// ============================================================================

/**
 * Handles file scanning, discovery, and importance scoring for LSP operations
 * with special support for Luau/Roblox projects
 */
export class FileScanner {
  private host: IFileScannerHost;
  private registry: IFileScannerRegistry;
  private rootUri: string;
  private isLuauProject: boolean = false;

  constructor(
    host: IFileScannerHost,
    registry: IFileScannerRegistry,
    rootUri: string
  ) {
    this.host = host;
    this.registry = registry;
    this.rootUri = rootUri;
    logger.info(`${LOGGER_PREFIX} Initialized for root: ${rootUri}`);
  }

  /**
   * Set whether this is a Luau project (affects skip logic and scoring)
   */
  setLuauProject(isLuau: boolean): void {
    this.isLuauProject = isLuau;
    logger.info(
      `${LOGGER_PREFIX} Luau project mode: ${isLuau ? "enabled" : "disabled"}`
    );
  }

  /**
   * Scan all files for the given languages
   * If no languages specified, uses available extensions from registry
   */
  async scan(languages?: string[]): Promise<FileStats> {
    const targetLanguages =
      languages ?? this.registry.getAvailableLanguageExtensions();
    logger.info(
      `${LOGGER_PREFIX} Scanning for ${targetLanguages.length} languages...`
    );

    const languageCounts: Record<string, number> = {};
    const allFiles: string[] = [];

    for (const language of targetLanguages) {
      const files = await this.findFilesByExtension(
        `.${language}`,
        MAX_FILES_PER_LANGUAGE
      );
      languageCounts[language] = files.length;
      allFiles.push(...files);
      logger.info(
        `${LOGGER_PREFIX} Found ${files.length} .${language} files`
      );
    }

    const entryPoints = await this.findEntryPoints(targetLanguages);

    const stats: FileStats = {
      totalFiles: allFiles.length,
      languages: targetLanguages,
      languageCounts,
      entryPoints,
    };

    logger.info(
      `${LOGGER_PREFIX} Scan complete: ${allFiles.length} total files, ${entryPoints.length} entry points`
    );

    return stats;
  }

  /**
   * Find entry points in the codebase, including Roblox-specific patterns for Luau projects
   */
  async findEntryPoints(languages: string[]): Promise<EntryPoint[]> {
    const entryPoints: EntryPoint[] = [];
    const seenPaths = new Set<string>();

    // Get all files for the given languages
    const allFiles: string[] = [];
    for (const language of languages) {
      const files = await this.findFilesByExtension(
        `.${language}`,
        MAX_FILES_PER_LANGUAGE
      );
      allFiles.push(...files);
    }

    // Check each file for entry point patterns
    for (const filePath of allFiles) {
      if (seenPaths.has(filePath)) continue;

      const fileName = filePath.split("/").pop() || "";
      const baseName = fileName.replace(/\.[^.]+$/, "");

      // Check for standard entry point files
      const isEntryPoint = ENTRY_POINT_FILES.includes(baseName.toLowerCase());
      const isRobloxEntry =
        this.isLuauProject && this.isRobloxEntryPoint(filePath);

      if (isEntryPoint || isRobloxEntry) {
        seenPaths.add(filePath);

        let type: EntryPoint["type"] = "main";
        if (isRobloxEntry) {
          type = "roblox";
        } else if (baseName.toLowerCase() === "lib") {
          type = "export";
        } else if (baseName.toLowerCase() === "cli" || baseName.toLowerCase() === "cmd") {
          type = "tool";
        }

        entryPoints.push({
          name: fileName,
          path: filePath,
          type,
        });

        logger.debug(
          `${LOGGER_PREFIX} Found entry point: ${fileName} (${type})`
        );
      }
    }

    // Try to use Rojo manifest for additional entry points (Luau projects)
    if (this.isLuauProject) {
      try {
        const rojoManifest = await this.parseRojoManifest();
        if (rojoManifest?.tree) {
          const rojoEntries = this.extractRojoEntryPoints(rojoManifest.tree);
          for (const entry of rojoEntries) {
            if (!seenPaths.has(entry.path)) {
              entryPoints.push(entry);
              seenPaths.add(entry.path);
            }
          }
        }
      } catch (e) {
        logger.debug(`${LOGGER_PREFIX} Failed to parse Rojo manifest: ${e}`);
      }
    }

    logger.info(
      `${LOGGER_PREFIX} Found ${entryPoints.length} entry points`
    );

    return entryPoints;
  }

  /**
   * Parse the Rojo manifest (default.project.json) for Luau projects
   */
  async parseRojoManifest(): Promise<RojoManifest | null> {
    const manifestPath = `${this.rootUri}/default.project.json`;

    try {
      const content = await this.host.readFile(manifestPath);
      const parsed = JSON.parse(content) as RojoManifest;

      if (!parsed.name || !parsed.tree) {
        logger.warn(
          `${LOGGER_PREFIX} Invalid Rojo manifest: missing name or tree`
        );
        return null;
      }

      logger.info(
        `${LOGGER_PREFIX} Parsed Rojo manifest for project: ${parsed.name}`
      );
      return parsed;
    } catch (e) {
      // Manifest doesn't exist or is invalid - this is fine
      logger.debug(`${LOGGER_PREFIX} No Rojo manifest found at ${manifestPath}`);
      return null;
    }
  }

  /**
   * Check if a file path matches Roblox entry point patterns
   */
  isRobloxEntryPoint(filePath: string): boolean {
    const fileName = filePath.split("/").pop() || "";
    return ROBLOX_ENTRY_PATTERNS.includes(fileName.toLowerCase());
  }

  /**
   * Score file importance based on various heuristics
   * Higher score = more important
   */
  scoreFileImportance(filePath: string, isLuauProject: boolean = this.isLuauProject): number {
    let score = 0;
    const fileName = filePath.split("/").pop()?.replace(/\.[^.]+$/, "") || "";
    const dirName = filePath.split("/").slice(-2, -1)[0] || "";
    const depth = filePath.split("/").length;

    // Entry point files are most important
    if (ENTRY_POINT_FILES.includes(fileName.toLowerCase())) {
      score += 100;
    }

    // Roblox-specific entry points for Luau
    if (isLuauProject && this.isRobloxEntryPoint(filePath)) {
      score += 30;
    }

    // Root proximity (closer to root = more important)
    score += Math.max(0, 20 - depth * 2);

    // Source directory bonus
    if (SOURCE_DIRS.includes(dirName.toLowerCase())) {
      score += 50;
    }

    // Low priority directory penalty
    if (LOW_PRIORITY_DIRS.includes(dirName.toLowerCase())) {
      score -= 30;
    }

    // Test files are lower priority
    if (
      fileName.includes("_test") ||
      fileName.includes(".test") ||
      fileName.includes(".spec")
    ) {
      score -= 50;
    }
    if (dirName.toLowerCase() === "test" || dirName.toLowerCase() === "tests") {
      score -= 40;
    }

    // Generated files are lowest priority
    if (
      filePath.includes("_gen") ||
      filePath.includes(".gen.") ||
      filePath.includes(".pb.")
    ) {
      score -= 80;
    }
    if (
      fileName.includes("generated") ||
      fileName.includes("auto") ||
      fileName.endsWith(".d.ts")
    ) {
      score -= 60;
    }

    // Luau-specific: Packages directory is NOT skipped, so give it a small bonus
    if (isLuauProject && dirName.toLowerCase() === "packages") {
      score += 10;
    }

    return score;
  }

  // ============================================================================
  // Private Methods
  // ============================================================================

  private async findFilesByExtension(
    ext: string,
    limit: number
  ): Promise<string[]> {
    try {
      const allFiles = await this.findAllFilesFast(ext, limit * 3);
      return this.prioritizeFiles(allFiles, this.isLuauProject).slice(0, limit);
    } catch (e) {
      const errMsg = e instanceof Error ? e.message : String(e);
      logger.warn(
        `${LOGGER_PREFIX} Failed to find files by extension ${ext}: ${errMsg}`
      );
      return [];
    }
  }

  private async findAllFilesFast(
    ext: string,
    limit: number
  ): Promise<string[]> {
    // Single find command to get all files recursively
    const { stdout } = await this.host.exec(
      `find . -type f -name "*${ext}" 2>/dev/null`,
      { cwd: this.rootUri }
    );
    let files = stdout
      .trim()
      .split("\n")
      .filter((f) => f);

    // Filter out directories that should be skipped
    files = files.filter((filePath) => {
      const parts = filePath.split("/");
      for (const part of parts) {
        const lower = part.toLowerCase();
        if (SKIP_DIRS.includes(lower)) {
          if (this.isLuauProject && LUAU_SKIP_EXCEPTIONS.includes(lower)) {
            continue; // don't skip if in exception list
          }
          return false;
        }
      }
      return true;
    });

    // Prioritize (sort by score) and apply limit
    return this.prioritizeFiles(files, this.isLuauProject).slice(0, limit);
  }

  private prioritizeFiles(
    files: string[],
    isLuauProject: boolean = this.isLuauProject
  ): string[] {
    return files
      .map((file) => ({
        file,
        score: this.scoreFileImportance(file, isLuauProject),
      }))
      .sort((a, b) => b.score - a.score)
      .map((f) => f.file);
  }

  private extractRojoEntryPoints(tree: Record<string, any>): EntryPoint[] {
    const entryPoints: EntryPoint[] = [];

    const traverse = (node: any, path: string): void => {
      if (!node) return;

      // If it's a file reference with a path
      if (node["$path"] && typeof node["$path"] === "string") {
        const targetPath = `${this.rootUri}/${node["$path"]}`;

        // Check if it matches entry point patterns
        const fileName = targetPath.split("/").pop() || "";
        const baseName = fileName.replace(/\.[^.]+$/, "");

        if (ENTRY_POINT_FILES.includes(baseName.toLowerCase())) {
          entryPoints.push({
            name: fileName,
            path: targetPath,
            type: "roblox",
          });
        }
      }

      // Traverse children
      if (node["$children"]) {
        for (const [childName, childNode] of Object.entries(node["$children"])) {
          traverse(childNode, `${path}/${childName}`);
        }
      }

      // Also traverse direct properties that look like nodes
      for (const [key, value] of Object.entries(node)) {
        if (!key.startsWith("$") && typeof value === "object") {
          traverse(value, `${path}/${key}`);
        }
      }
    };

    traverse(tree, "");
    return entryPoints;
  }
}
