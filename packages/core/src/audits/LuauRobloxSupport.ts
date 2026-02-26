import { LSPUtility, type CodebaseStats } from '../lsp/LSPUtility';
import { logger } from '@firmius/shared/utils/logger';
import type { IHost } from '@firmius/shared';
import type { EntryPoint } from '../lsp/FileScanner';
import { LocalHost } from '@firmius/core/hosts';

const LOG_PREFIX = '[LuauAudit]';

interface AuditResult {
  test: string;
  passed: boolean;
  details: string;
}

async function runLuauAudit(projectPath: string): Promise<AuditResult[]> {
  const results: AuditResult[] = [];
  const host: IHost = new LocalHost();
  const lsp = new LSPUtility(host, projectPath);

  logger.info(`${LOG_PREFIX} Starting Luau/Roblox support audit for: ${projectPath}`);

  // Scan once and share stats across all tests
  logger.info(`${LOG_PREFIX} Performing initial codebase scan...`);
  const stats = await lsp.scan();
  logger.info(`${LOG_PREFIX} Scan complete. Running tests...`);

  // Test 1: Language Detection
  logger.info(`${LOG_PREFIX} Test 1: Language Detection`);
  try {
    results.push(await testLanguageDetection(stats));
  } catch (e) {
    results.push({ test: 'Language Detection', passed: false, details: `Error: ${e}` });
  }

  // Test 2: File Discovery
  logger.info(`${LOG_PREFIX} Test 2: File Discovery`);
  try {
    results.push(await testFileDiscovery(stats));
  } catch (e) {
    results.push({ test: 'File Discovery', passed: false, details: `Error: ${e}` });
  }

  // Test 3: Entry Point Detection
  logger.info(`${LOG_PREFIX} Test 3: Entry Point Detection`);
  try {
    results.push(await testEntryPointDetection(stats));
  } catch (e) {
    results.push({ test: 'Entry Point Detection', passed: false, details: `Error: ${e}` });
  }

  // Test 4: Symbol Indexing
  logger.info(`${LOG_PREFIX} Test 4: Symbol Indexing`);
  try {
    results.push(await testSymbolIndexing(lsp));
  } catch (e) {
    results.push({ test: 'Symbol Indexing', passed: false, details: `Error: ${e}` });
  }

  // Test 5: Rojo Manifest Parsing
  logger.info(`${LOG_PREFIX} Test 5: Rojo Manifest Parsing`);
  try {
    results.push(await testRojoManifestParsing(lsp));
  } catch (e) {
    results.push({ test: 'Rojo Manifest Parsing', passed: false, details: `Error: ${e}` });
  }

  // Test 6: LSP Operations (if Luau files exist)
  logger.info(`${LOG_PREFIX} Test 6: LSP Operations`);
  try {
    results.push(await testLSPOperations(lsp, stats));
  } catch (e) {
    results.push({ test: 'LSP Operations', passed: false, details: `Error: ${e}` });
  }

  // Cleanup
  await lsp.dispose();

  return results;
}

async function testLanguageDetection(stats: CodebaseStats): Promise<AuditResult> {
  const hasLuau = stats.languages.has('luau') || stats.languages.has('lua');
  return {
    test: 'Language Detection',
    passed: hasLuau,
    details: hasLuau ? `Detected Luau/Lua languages: ${Array.from(stats.languages.keys()).join(', ')}` : 'No Luau/Lua languages detected'
  };
}

async function testFileDiscovery(stats: CodebaseStats): Promise<AuditResult> {
  const hasLuaFiles = stats.totalFiles > 0;
  return {
    test: 'File Discovery',
    passed: hasLuaFiles,
    details: `Found ${stats.totalFiles} total files`
  };
}

async function testEntryPointDetection(stats: CodebaseStats): Promise<AuditResult> {
  const hasEntryPoints = stats.entryPoints.length > 0;
  const robloxEntryPoints = stats.entryPoints.filter((ep: EntryPoint) => ep.type === 'roblox');
  return {
    test: 'Entry Point Detection',
    passed: hasEntryPoints,
    details: `Found ${stats.entryPoints.length} entry points (${robloxEntryPoints.length} Roblox-specific)`
  };
}

async function testSymbolIndexing(lsp: LSPUtility): Promise<AuditResult> {
  const symbols = lsp.getTopSymbols(10);
  const hasSymbols = symbols.length > 0;
  return {
    test: 'Symbol Indexing',
    passed: hasSymbols,
    details: `Indexed ${symbols.length} top symbols`
  };
}

async function testRojoManifestParsing(lsp: LSPUtility): Promise<AuditResult> {
  // Access fileScanner through internal property
  const lspAny = lsp as any;
  const fs = lspAny.fileScanner;
  if (!fs) {
    return { test: 'Rojo Manifest Parsing', passed: false, details: 'Could not access fileScanner' };
  }
  const rojoManifest = await fs.parseRojoManifest();
  const hasRojo = rojoManifest !== null;
  return {
    test: 'Rojo Manifest Parsing',
    passed: true,
    details: hasRojo ? `Parsed Rojo manifest: ${rojoManifest.name}` : 'No Rojo manifest found (optional)'
  };
}

async function testLSPOperations(lsp: LSPUtility, stats: CodebaseStats): Promise<AuditResult> {
  const luauFiles = stats.entryPoints
    .filter((ep: EntryPoint) => ep.path.endsWith('.luau') || ep.path.endsWith('.lua'))
    .map((ep: EntryPoint) => ep.path);
  
  if (luauFiles.length > 0) {
    const testFile = luauFiles[0];
    if (!testFile) {
      return {
        test: 'LSP Operations',
        passed: false,
        details: 'No valid Luau file path found'
      };
    }
    const symbols = await lsp.getDocumentSymbols(testFile);
    return {
      test: 'LSP Operations',
      passed: true,
      details: `Retrieved ${symbols.length} symbols from ${testFile}`
    };
  } else {
    return {
      test: 'LSP Operations',
      passed: true,
      details: 'No Luau files available for LSP testing (skipped)'
    };
  }
}

function printResults(results: AuditResult[]): void {
  logger.info(`${LOG_PREFIX} =========================================`);
  logger.info(`${LOG_PREFIX} AUDIT RESULTS`);
  logger.info(`${LOG_PREFIX} =========================================`);
  
  let passed = 0;
  let failed = 0;
  
  for (const result of results) {
    const status = result.passed ? '✓ PASS' : '✗ FAIL';
    logger.info(`${LOG_PREFIX} ${status}: ${result.test}`);
    logger.info(`${LOG_PREFIX}   ${result.details}`);
    
    if (result.passed) passed++;
    else failed++;
  }
  
  logger.info(`${LOG_PREFIX} -----------------------------------------`);
  logger.info(`${LOG_PREFIX} Total: ${passed} passed, ${failed} failed`);
  logger.info(`${LOG_PREFIX} =========================================`);
  
  if (failed > 0) {
    process.exit(1);
  }
}

async function main() {
  const projectPath = process.argv[2] || process.cwd();
  
  logger.info(`${LOG_PREFIX} Luau/Roblox Support Audit`);
  logger.info(`${LOG_PREFIX} Project: ${projectPath}`);
  logger.info(`${LOG_PREFIX} =========================================`);
  
  const results = await runLuauAudit(projectPath);
  printResults(results);
}

main().catch(e => {
  logger.error(`${LOG_PREFIX} Fatal error: ${e}`);
  process.exit(1);
});
