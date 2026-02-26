import { LSPUtility } from "@firmius/core/lsp";
import { Engine } from "@firmius/core";
import { LocalHost } from "@firmius/core/hosts";
import { logger } from "@firmius/shared";

// Projects to test - we'll break each one differently
// Note: expectedLangs should match the language names returned by getLanguageFromPath()
const TEST_PROJECTS = [
  { path: "/mnt/SHIT/Projects/AIShitLol", name: "AIShitLol", type: "cpp", expectedLangs: ["cpp", "c"], breakMode: "third_party_flood" },
  { path: "/mnt/SHIT/Projects/vinegar", name: "vinegar", type: "go", expectedLangs: ["go"], breakMode: "deep_nesting" },
  { path: "/mnt/SHIT/Projects/FrameworkLuau", name: "FrameworkLuau", type: "luau", expectedLangs: ["luau"], breakMode: "symbol_explosion" },
  { path: "/mnt/SHIT/Projects/FrameworkTS", name: "FrameworkTS", type: "ts", expectedLangs: ["typescript"], breakMode: "monorepo_chaos" },
  { path: "/mnt/SHIT/Projects/Reflex", name: "Reflex", type: "ts", expectedLangs: ["typescript"], breakMode: "rapid_scan" },
  { path: "/mnt/SHIT/Projects/Commisions/Fleshline", name: "Fleshline", type: "ts", expectedLangs: ["typescript"], breakMode: "roblox_ts" },
  { path: "/mnt/SHIT/Projects/Commisions/FleshlineOld", name: "FleshlineOld", type: "luau", expectedLangs: ["luau"], breakMode: "legacy_luau" },
  { path: "/mnt/SHIT/Projects/Commisions/KzFlix", name: "KzFlix", type: "ts", expectedLangs: ["typescript"], breakMode: "api_noise" },
];

await Engine.ignite();
const Local = new LocalHost();

interface BreakageResult {
  project: string;
  breakMode: string;
  survived: boolean;
  errors: string[];
  warnings: string[];
  metrics: {
    scanTime: number;
    languagesDetected: string[];
    expectedLanguages: string[];
    symbolsIndexed: number;
    thirdPartyFiles: number;
    projectFiles: number;
    topFileIsThirdParty: boolean;
    referenceGraphEdges: number;
    memoryBefore?: number;
    memoryAfter?: number;
  };
}

const results: BreakageResult[] = [];

async function breakProject(project: typeof TEST_PROJECTS[0]): Promise<BreakageResult> {
  logger.info(`\n${"=".repeat(60)}`);
  logger.info(`BREAKING: ${project.name} (${project.breakMode})`);
  logger.info(`${"=".repeat(60)}\n`);

  const errors: string[] = [];
  const warnings: string[] = [];
  const memBefore = process.memoryUsage();

    const startTime = Date.now();
    let survived = true;
    let cbi: LSPUtility | null = null;

  try {
     cbi = Engine.getLSPUtility(Local, project.path);

     // Normal scan with force refresh
      const stats = await cbi!.scan(true);
      const scanTime = Date.now() - startTime;

      // Check top files
     const topFiles = cbi!.getTopFiles(20);
     const thirdPartyFiles = topFiles.filter(f =>
       f.path.includes('third_party') ||
       f.path.includes('node_modules') ||
       f.path.includes('vendor')
     );
     const projectFiles = topFiles.filter(f => !thirdPartyFiles.some(tp => tp.path === f.path));

     // Get reference graph summary
     const refSummary = cbi!.getReferenceSummary(10);

    const memAfter = process.memoryUsage();

    const result: BreakageResult = {
      project: project.name,
      breakMode: project.breakMode,
      survived,
      errors,
      warnings,
        metrics: {
          scanTime,
          languagesDetected: Array.from(stats.languages.keys()),
          expectedLanguages: project.expectedLangs,
          symbolsIndexed: cbi!.getSymbolCount(),
          thirdPartyFiles: thirdPartyFiles.length,
          projectFiles: projectFiles.length,
        topFileIsThirdParty: thirdPartyFiles.length > 0 && topFiles[0]?.path === thirdPartyFiles[0]?.path,
        referenceGraphEdges: refSummary.reduce((sum, r) => sum + r.outgoingRefs + r.incomingRefs, 0),
        memoryBefore: Math.round(memBefore.heapUsed / 1024 / 1024),
        memoryAfter: Math.round(memAfter.heapUsed / 1024 / 1024),
      }
    };

    // Log findings
    logger.info(`\n[BREAKER] Results for ${project.name}:`);
    logger.info(`  Scan time: ${scanTime}ms`);
    logger.info(`  Languages detected: ${result.metrics.languagesDetected.join(', ') || 'NONE'}`);
    logger.info(`  Expected: ${result.metrics.expectedLanguages.join(', ')}`);
    logger.info(`  Symbols indexed: ${result.metrics.symbolsIndexed}`);
    logger.info(`  Top 20 files: ${result.metrics.projectFiles} project, ${result.metrics.thirdPartyFiles} third-party`);
    logger.info(`  Third-party in top spot: ${result.metrics.topFileIsThirdParty ? 'YES (BAD)' : 'NO (GOOD)'}`);
    logger.info(`  Reference graph edges: ${result.metrics.referenceGraphEdges}`);
    logger.info(`  Memory: ${result.metrics.memoryBefore}MB -> ${result.metrics.memoryAfter}MB (${result.metrics.memoryAfter! - result.metrics.memoryBefore!}MB delta)`);

    // Check for issues
    if (result.metrics.languagesDetected.length === 0) {
      errors.push("NO LANGUAGES DETECTED");
    }
    if (result.metrics.topFileIsThirdParty) {
      errors.push("THIRD-PARTY FILE RANKED HIGHER THAN PROJECT FILES");
    }
    if (result.metrics.thirdPartyFiles > 10) {
      warnings.push(`${result.metrics.thirdPartyFiles} third-party files in top 20`);
    }
    if (result.metrics.memoryAfter! - result.metrics.memoryBefore! > 100) {
      warnings.push(`High memory usage: ${result.metrics.memoryAfter! - result.metrics.memoryBefore!}MB delta`);
    }
    // 0 symbols could mean empty files (like Reflex/src/index.ts) - not necessarily an error
    if (result.metrics.symbolsIndexed === 0 && result.metrics.languagesDetected.length > 0) {
      warnings.push("0 symbols indexed - files may be empty or LSP failed to parse");
    }

    // List top 5 files
    logger.info(`\n[BREAKER] Top 5 files:`);
    topFiles.slice(0, 5).forEach((f, i) => {
      const isTp = f.path.includes('third_party') || f.path.includes('node_modules');
      logger.info(`  ${i + 1}. ${isTp ? '[TP] ' : ''}${f.path.split('/').pop()} (score: ${f.score})`);
    });

    return result;

  } catch (e) {
    survived = false;
    const errorMsg = e instanceof Error ? e.message : String(e);
    errors.push(`CRASH: ${errorMsg}`);
    logger.error(`[BREAKER] ${project.name} CRASHED: ${errorMsg}`);

    return {
      project: project.name,
      breakMode: project.breakMode,
      survived: false,
      errors,
      warnings,
      metrics: {
        scanTime: Date.now() - startTime,
        languagesDetected: [],
        expectedLanguages: project.expectedLangs,
        symbolsIndexed: 0,
        thirdPartyFiles: 0,
        projectFiles: 0,
        topFileIsThirdParty: false,
        referenceGraphEdges: 0,
      }
    };
  } finally {
    // CRITICAL: Test cleanup - this is where memory leaks happen!
    if (cbi) {
      logger.info(`[BREAKER] Testing cleanup for ${project.name}...`);
      try {
        await cbi.dispose();
        logger.info(`[BREAKER] Cleanup successful`);
      } catch (e) {
        const errorMsg = e instanceof Error ? e.message : String(e);
        errors.push(`CLEANUP FAILED: ${errorMsg}`);
        logger.error(`[BREAKER] Cleanup failed: ${errorMsg}`);
      }
    }
  }
}

async function runMemoryLeakTest(): Promise<void> {
  logger.info(`\n${"=".repeat(60)}`);
  logger.info("MEMORY LEAK TORTURE TEST");
  logger.info(`${"=".repeat(60)}\n`);

  const project = TEST_PROJECTS[0]!;
  const memSnapshots: number[] = [];

   for (let i = 0; i < 10; i++) {
     const cbi = Engine.getLSPUtility(Local, project.path);
     await cbi.scan(true);

     // Force GC if available
    if (global.gc) {
      global.gc();
    }

    const mem = process.memoryUsage().heapUsed / 1024 / 1024;
    memSnapshots.push(Math.round(mem));
    logger.info(`[LEAK TEST] Iteration ${i + 1}: ${Math.round(mem)}MB`);

    // Dispose
    await cbi.dispose();

    // Small delay
    await new Promise(r => setTimeout(r, 100));
  }

  logger.info(`\n[LEAK TEST] Memory snapshots: ${memSnapshots.join(' -> ')}MB`);
  const last = memSnapshots[memSnapshots.length - 1] ?? 0;
  const first = memSnapshots[0] ?? 0;
  const growth = last - first;
  logger.info(`[LEAK TEST] Total growth: ${growth}MB ${growth > 50 ? '⚠️ POTENTIAL LEAK' : '✓ OK'}`);
}

async function main() {
  logger.info(`\n${"#".repeat(60)}`);
  logger.info("# LSP ROBUSTNESS BREAKAGE SUITE");
  logger.info("# Purpose: Break CodebaseIntelligence on purpose");
  logger.info(`#".repeat(60)}\n`);

  // Test each project
  for (const project of TEST_PROJECTS) {
    const result = await breakProject(project);
    results.push(result);
  }

  // Run memory leak test
  await runMemoryLeakTest();

  // Final report
  logger.info(`\n${"=".repeat(60)}`);
  logger.info("FINAL BREAKAGE REPORT");
  logger.info(`${"=".repeat(60)}\n`);

  const survived = results.filter(r => r.survived);
  const crashed = results.filter(r => !r.survived);
  const withErrors = results.filter(r => r.errors.length > 0);
  const withThirdPartyIssues = results.filter(r => r.metrics.topFileIsThirdParty);

  logger.info(`Projects tested: ${results.length}`);
  logger.info(`Survived: ${survived.length}`);
  logger.info(`Crashed: ${crashed.length}`);
  logger.info(`With errors: ${withErrors.length}`);
  logger.info(`Third-party ranking issues: ${withThirdPartyIssues.length}`);

  logger.info(`\n--- Projects with Issues ---`);
  for (const r of withErrors) {
    logger.info(`\n${r.project} (${r.breakMode}):`);
    r.errors.forEach(e => logger.info(`  ❌ ${e}`));
    r.warnings.forEach(w => logger.info(`  ⚠️  ${w}`));
  }

  logger.info(`\n--- Language Detection Issues ---`);
  for (const r of results) {
    const missing = r.metrics.expectedLanguages.filter(
      lang => !r.metrics.languagesDetected.includes(lang)
    );
    if (missing.length > 0) {
      logger.info(`  ${r.project}: Missing ${missing.join(', ')} (found: ${r.metrics.languagesDetected.join(', ') || 'none'})`);
    }
  }

  logger.info(`\n${"=".repeat(60)}`);
  logger.info("BREAKAGE COMPLETE");
  logger.info(`${"=".repeat(60)}\n`);
}

main().catch(e => {
  logger.error(`FATAL: ${e instanceof Error ? e.message : String(e)}`);
  process.exit(1);
});
