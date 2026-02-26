import { logger } from '@firmius/shared/utils';

/**
 * Host interface for file system operations
 */
export interface IDetectorHost {
  readFile(path: string): Promise<string>;
  exec(command: string, options?: any): Promise<{ stdout: string }>;
}

/**
 * Registry interface for getting available language extensions
 */
export interface IDetectorRegistry {
  getAvailableLanguageExtensions(): string[];
}

/**
 * Result of language detection for a single extension
 */
export interface DetectionResult {
  ext: string;
  found: boolean;
  reason: string;
}

/**
 * Project configuration files that hint at language usage
 * Maps config file names to associated language extensions
 */
const PROJECT_LANGUAGE_HINTS: Record<string, string[]> = {
  'go.mod': ['go'],
  'Cargo.toml': ['rs'],
  'package.json': ['ts', 'js'],
  'tsconfig.json': ['ts', 'js'],
  'pyproject.toml': ['py'],
  'setup.py': ['py'],
  'requirements.txt': ['py'],
  'CMakeLists.txt': ['cpp', 'c'],
  'Makefile': ['cpp', 'c'],
  'default.project.json': ['luau', 'lua'], // Rojo manifest
  'aftman.toml': ['luau', 'lua'],
  'rokit.toml': ['luau', 'lua'],
  'wally.toml': ['luau', 'lua'],
  '.luaurc': ['luau'],
  '.luacheckrc': ['lua'],
};

/**
 * File extensions associated with Luau/Roblox projects
 */
const LUAU_EXTENSIONS = ['luau', 'lua'];

/**
 * Maximum number of files to check when scanning for extensions
 */
const FILES_TO_CHECK = 10;

/**
 * Logger prefix for all LanguageDetector log messages
 */
const LOG_PREFIX = '[LSPUtility:LanguageDetector]';

/**
 * LanguageDetector handles detection of programming languages in a project
 * by analyzing file extensions, configuration files, and project structure.
 * Includes special support for Luau/Roblox projects.
 */
export class LanguageDetector {
  private host: IDetectorHost;
  private registry: IDetectorRegistry;
  private rootUri: string;

  /**
   * Creates a new LanguageDetector instance
   * @param host - Host providing file system operations
   * @param registry - Registry providing available language extensions
   * @param rootUri - Root URI of the project
   */
  constructor(host: IDetectorHost, registry: IDetectorRegistry, rootUri: string) {
    this.host = host;
    this.registry = registry;
    this.rootUri = rootUri;
    logger.debug(`${LOG_PREFIX} Initialized with root: ${rootUri}`);
  }

  /**
   * Detects all languages present in the project
   * Checks project hints first, then scans for file extensions
   * @returns Promise resolving to array of detected language extensions
   */
  async detectLanguages(): Promise<string[]> {
    logger.info(`${LOG_PREFIX} Starting language detection in ${this.rootUri}`);
    const detectedLanguages: Set<string> = new Set();
    const availableExtensions = this.registry.getAvailableLanguageExtensions();

    logger.debug(`${LOG_PREFIX} Available extensions from registry: ${availableExtensions.join(', ')}`);

    // First check project hints (config files)
    for (const [configFile, languages] of Object.entries(PROJECT_LANGUAGE_HINTS)) {
      const configPath = `${this.rootUri}/${configFile}`;
      try {
        await this.host.readFile(configPath);
        logger.info(`${LOG_PREFIX} Found project config: ${configFile} → languages: ${languages.join(', ')}`);
        
        // Special handling for TypeScript - require tsconfig.json
        if (languages.includes('ts') && configFile === 'tsconfig.json') {
          languages.forEach(lang => detectedLanguages.add(lang));
        } else if (!languages.includes('ts')) {
          languages.forEach(lang => detectedLanguages.add(lang));
        }
      } catch {
        // Config file not found, continue
      }
    }

    // Check for Luau project specifically
    const isLuau = await this.isLuauProject();
    if (isLuau) {
      logger.info(`${LOG_PREFIX} Luau project detected, adding luau and lua extensions`);
      detectedLanguages.add('luau');
      detectedLanguages.add('lua');
    }

    // Scan for files by extension for remaining undetected languages
    for (const ext of availableExtensions) {
      if (detectedLanguages.has(ext)) {
        continue; // Already detected via config file
      }

      const result = await this.detectLanguage(ext);
      if (result.found) {
        logger.info(`${LOG_PREFIX} Detected language: ${ext} - ${result.reason}`);
        detectedLanguages.add(ext);
      }
    }

    const result = Array.from(detectedLanguages);
    logger.info(`${LOG_PREFIX} Language detection complete. Found: ${result.join(', ') || 'none'}`);
    return result;
  }

  /**
   * Detects if a specific language extension is present in the project
   * @param ext - File extension to check (without dot)
   * @returns Promise resolving to detection result
   */
  async detectLanguage(ext: string): Promise<DetectionResult> {
    logger.debug(`${LOG_PREFIX} Detecting language: ${ext}`);

    // Check project hints first
    for (const [configFile, languages] of Object.entries(PROJECT_LANGUAGE_HINTS)) {
      if (languages.includes(ext)) {
        const hasConfig = await this.hasProjectConfig([configFile]);
        if (hasConfig) {
          return {
            ext,
            found: true,
            reason: `Found project config: ${configFile}`,
          };
        }
      }
    }

    // Check for files with this extension
    const files = await this.findFilesByExtension(ext, FILES_TO_CHECK);
    if (files.length > 0) {
      return {
        ext,
        found: true,
        reason: `Found ${files.length} file(s) with .${ext} extension`,
      };
    }

    return {
      ext,
      found: false,
      reason: `No files or config found for .${ext}`,
    };
  }

  /**
   * Checks if any of the specified configuration files exist in the project
   * @param configFiles - Array of config file names to check
   * @returns Promise resolving to true if any config file exists
   */
  async hasProjectConfig(configFiles: string[]): Promise<boolean> {
    for (const configFile of configFiles) {
      const configPath = `${this.rootUri}/${configFile}`;
      try {
        await this.host.readFile(configPath);
        logger.debug(`${LOG_PREFIX} Found project config: ${configFile}`);
        return true;
      } catch {
        // Config file not found, continue checking
      }
    }
    return false;
  }

  /**
   * Determines if this is a Luau/Roblox project by checking for:
   * - Rojo manifest (default.project.json)
   * - Luau toolchain configs (aftman.toml, rokit.toml, wally.toml)
   * - Presence of both .lua and .luau files
   * @returns Promise resolving to true if this is a Luau project
   */
  async isLuauProject(): Promise<boolean> {
    logger.debug(`${LOG_PREFIX} Checking for Luau project indicators`);

    // Check for Luau-specific config files
    const luauConfigs = [
      'default.project.json', // Rojo manifest
      'aftman.toml',
      'rokit.toml',
      'wally.toml',
      '.luaurc',
    ];

    for (const config of luauConfigs) {
      const hasConfig = await this.hasProjectConfig([config]);
      if (hasConfig) {
        logger.info(`${LOG_PREFIX} Luau project detected via config: ${config}`);
        return true;
      }
    }

    // Check for Luau-related files using LUAU_EXTENSIONS constant
    const foundExtensions: string[] = [];
    for (const ext of LUAU_EXTENSIONS) {
      const files = await this.findFilesByExtension(ext, 1);
      if (files.length > 0) {
        foundExtensions.push(ext);
      }
    }

    if (foundExtensions.length > 0) {
      if (foundExtensions.length === LUAU_EXTENSIONS.length) {
        logger.info(`${LOG_PREFIX} Luau project detected: found both .lua and .luau files`);
      } else if (foundExtensions.includes('luau')) {
        logger.info(`${LOG_PREFIX} Luau project detected: found .luau files`);
      } else {
        logger.info(`${LOG_PREFIX} Lua project detected: found .lua files`);
      }

      return true;
    }

    logger.debug(`${LOG_PREFIX} No Luau project indicators found`);
    return false;
  }

  /**
   * Finds files with the specified extension in the project
   * @param ext - File extension to search for (without dot)
   * @param limit - Maximum number of files to find (default: FILES_TO_CHECK)
   * @returns Promise resolving to array of file paths
   */
  async findFilesByExtension(ext: string, limit: number = FILES_TO_CHECK): Promise<string[]> {
    logger.debug(`${LOG_PREFIX} Searching for .${ext} files (limit: ${limit})`);
    
    try {
      // Use find command to locate files with the specified extension
      const pattern = `*.${ext}`;
      const { stdout } = await this.host.exec(
        `find "${this.rootUri}" -type f -name "${pattern}" 2>/dev/null | head -n ${limit}`,
        { cwd: this.rootUri }
      );
      
      const files = stdout
        .split('\n')
        .map(line => line.trim())
        .filter(line => line.length > 0);
      
      logger.debug(`${LOG_PREFIX} Found ${files.length} .${ext} file(s)`);
      return files;
    } catch (error) {
      logger.debug(`${LOG_PREFIX} Error finding .${ext} files: ${error}`);
      return [];
    }
  }
}
