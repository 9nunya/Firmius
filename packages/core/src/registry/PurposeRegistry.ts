import { readdir, readFile, access } from "node:fs/promises";
import { join } from "node:path";
import { homedir } from "node:os";
import { parse as parseYaml } from "yaml";
import { logger } from "@firmius/shared/utils";

// Global interface for embedded purposes (set by build script)
declare global {
  var EMBEDDED_PURPOSES: PurposeDefinition[] | undefined;
}

const LOGGER_PREFIX = "[PurposeRegistry]";
const FRONTMATTER_DELIMITER = "---";
const PURPOSE_FILE_EXTENSION = ".md";

const DEFAULTS_DIR = join(process.cwd(), "prompts");
const USER_PURPOSES_DIR = join(homedir(), ".firmius", "purposes");

const REQUIRED_FRONTMATTER_FIELDS = [
  "name",
  "title",
  "description",
  "scopes",
] as const;

interface PurposeFrontmatter {
  name: string;
  title: string;
  description: string;
  scopes: string[];
  canSpawn?: string[];
}

export interface PurposeDefinition {
  /** Unique identifier (e.g., "architect") */
  name: string;
  /** Human-readable title (e.g., "System Architect") */
  title: string;
  /** Short description of what this purpose does */
  description: string;
  /** Tool scopes this purpose has access to (as string values matching ToolScope enum) */
  scopes: string[];
  /** List of purpose names this agent can spawn as sub-agents */
  canSpawn: string[];
  /** The markdown body — injected as as agent's system identity prompt */
  systemPrompt: string;
}

export class PurposeRegistry {
  private purposes: Map<string, PurposeDefinition> = new Map();
  private initialized = false;

  async init(useFilesystem = true): Promise<void> {
    if (this.initialized) return;

    // Try to load embedded purposes first (from build-time embedding)
    if (this.loadEmbeddedPurposes()) {
      logger.info(
        `${LOGGER_PREFIX} Loaded embedded purposes`,
      );
      this.initialized = true;
      return;
    }

    // Fall back to filesystem if embedding failed or in dev mode
    if (useFilesystem) {
      await this.loadFromDirectory(DEFAULTS_DIR);
      const defaultCount = this.purposes.size;

      await this.loadFromDirectory(USER_PURPOSES_DIR);
      const userOverrides = this.purposes.size - defaultCount;

      logger.info(
        `${LOGGER_PREFIX} Loaded ${this.purposes.size} purposes (${defaultCount} defaults, ${userOverrides} user overrides)`,
      );
    }

    this.initialized = true;
  }

  private loadEmbeddedPurposes(): boolean {
    try {
      // Try to import embedded purposes
      const embeddedPurposes = globalThis.EMBEDDED_PURPOSES;
      if (embeddedPurposes && Array.isArray(embeddedPurposes)) {
        this.loadPurposes(embeddedPurposes);
        return true;
      }
    } catch (err) {
      // Embedded purposes not available (dev mode)
      logger.debug(`${LOGGER_PREFIX} No embedded purposes found, loading from filesystem`);
    }
    return false;
  }

  loadPurposes(purposes: PurposeDefinition[]): void {
    for (const purpose of purposes) {
      this.purposes.set(purpose.name, purpose);
    }
    this.initialized = true;
    logger.info(
      `${LOGGER_PREFIX} Loaded ${purposes.length} embedded purposes`,
    );
  }

  getPurpose(name: string): PurposeDefinition | undefined {
    return this.purposes.get(name);
  }

  listPurposes(): PurposeDefinition[] {
    return Array.from(this.purposes.values());
  }

  private async loadFromDirectory(dirPath: string): Promise<void> {
    try {
      await access(dirPath);
    } catch {
      logger.debug(`${LOGGER_PREFIX} Directory not found, skipping: ${dirPath}`);
      return;
    }

    const entries = await readdir(dirPath);
    const mdFiles = entries.filter((f) => f.endsWith(PURPOSE_FILE_EXTENSION));

    for (const file of mdFiles) {
      const filePath = join(dirPath, file);
      try {
        const content = await readFile(filePath, "utf-8");
        const definition = this.parsePurposeFile(content);
        this.purposes.set(definition.name, definition);
        logger.debug(
          `${LOGGER_PREFIX} Loaded purpose "${definition.name}" from ${filePath}`,
        );
      } catch (err) {
        logger.warn(
          `${LOGGER_PREFIX} Failed to parse ${filePath}: ${err instanceof Error ? err.message : String(err)}`,
        );
      }
    }
  }

  private parsePurposeFile(content: string): PurposeDefinition {
    const trimmed = content.trimStart();
    if (!trimmed.startsWith(FRONTMATTER_DELIMITER)) {
      throw new Error("Missing YAML frontmatter — file must start with ---");
    }

    const afterFirstDelimiter = trimmed.slice(FRONTMATTER_DELIMITER.length);
    const closingIndex = afterFirstDelimiter.indexOf(
      `\n${FRONTMATTER_DELIMITER}`,
    );
    if (closingIndex === -1) {
      throw new Error("Missing closing --- for YAML frontmatter");
    }

    const yamlBlock = afterFirstDelimiter.slice(0, closingIndex);
    const body = afterFirstDelimiter
      .slice(closingIndex + 1 + FRONTMATTER_DELIMITER.length)
      .trim();

    const frontmatter = parseYaml(yamlBlock) as PurposeFrontmatter;

    for (const field of REQUIRED_FRONTMATTER_FIELDS) {
      if (frontmatter[field] === undefined || frontmatter[field] === null) {
        throw new Error(`Missing required frontmatter field: "${field}"`);
      }
    }

    if (!Array.isArray(frontmatter.scopes)) {
      throw new Error(`"scopes" must be an array`);
    }

    return {
      name: frontmatter.name,
      title: frontmatter.title,
      description: frontmatter.description,
      scopes: frontmatter.scopes,
      canSpawn: Array.isArray(frontmatter.canSpawn)
        ? frontmatter.canSpawn
        : [],
      systemPrompt: body,
    };
  }
}

export const purposeRegistry = new PurposeRegistry();
