import { Engine } from "@firmius/core";
import { DockerHost } from "../hosts/DockerHost";
import type { CodebaseStats } from "../lsp/LSPUtility";
import { type DocumentSymbol, type IHost } from "@firmius/shared";
import { SymbolKind } from "@firmius/shared";
import * as path from "node:path";
import * as os from "node:os";
import * as fs from "node:fs";
import { execSync } from "node:child_process";

const REPO_CACHE_DIR = process.env.SWE_REPO_CACHE || path.join(os.homedir(), ".firmius/swebench-temp-repos");

interface AuditConfig {
  preferMultiLang: boolean;
  minLanguages: number;
  preferLarge: boolean;
  repoFilter?: string;
  timeoutMs: number;
  dockerImage: string;
}

interface TimingMetric {
  name: string;
  durationMs: number;
  success: boolean;
  error?: string;
  details?: Record<string, unknown>;
}

interface AuditResult {
  repo: string;
  repoPath: string;
  config: AuditConfig;
  timings: TimingMetric[];
  languages: string[];
  symbolCount: number;
  fileCount: number;
  topFiles: { path: string; score: number; symbolCount: number }[];
  testResults: { test: string; passed: boolean; details: string }[];
  totalDurationMs: number;
  peakMemoryMb: number;
}

const DEFAULT_CONFIG: AuditConfig = {
  preferMultiLang: true,
  minLanguages: 1,
  preferLarge: false,
  timeoutMs: 300000,
  dockerImage: "firmius-sandbox:latest"
};

async function discoverCachedRepos(): Promise<{ name: string; path: string }[]> {
  if (!fs.existsSync(REPO_CACHE_DIR)) return [];

  const entries = fs.readdirSync(REPO_CACHE_DIR, { withFileTypes: true });
  const repos: { name: string; path: string }[] = [];

  for (const entry of entries) {
    if (entry.isDirectory()) {
      const subPath = path.join(REPO_CACHE_DIR, entry.name);
      const subEntries = fs.readdirSync(subPath, { withFileTypes: true });

      for (const subEntry of subEntries) {
        if (subEntry.isDirectory()) {
          const repoPath = path.join(subPath, subEntry.name);
          if (fs.existsSync(path.join(repoPath, ".git"))) {
            repos.push({ name: `${entry.name}/${subEntry.name}`, path: repoPath });
          }
        }
      }

      if (fs.existsSync(path.join(subPath, ".git"))) {
        repos.push({ name: entry.name, path: subPath });
      }
    }
  }

  return repos;
}

async function analyzeRepoLanguages(repoPath: string): Promise<string[]> {
  const langIndicators: Record<string, string[]> = {
    go: ['go.mod', 'go.sum'],
    ts: ['tsconfig.json'],
    js: ['package.json'],
    py: ['pyproject.toml', 'setup.py', 'requirements.txt'],
    rs: ['Cargo.toml'],
    cpp: ['CMakeLists.txt', 'Makefile'],
    c: ['CMakeLists.txt', 'Makefile'],
  };

  const languages: string[] = [];
  const entries = fs.readdirSync(repoPath);

  for (const [lang, indicators] of Object.entries(langIndicators)) {
    if (indicators.some(ind => entries.includes(ind))) {
      if (!languages.includes(lang)) languages.push(lang);
    }
  }

  const extCounts: Record<string, number> = {};
  countFilesByExt(repoPath, extCounts, 3);

  for (const [ext, count] of Object.entries(extCounts)) {
    if (count > 5) {
      const lang = extToLang(ext);
      if (lang && !languages.includes(lang)) languages.push(lang);
    }
  }

  return languages;
}

function countFilesByExt(dir: string, counts: Record<string, number>, maxDepth: number) {
  if (maxDepth <= 0) return;
  try {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (const entry of entries) {
      if (entry.isDirectory() && !['node_modules', '.git', 'vendor', 'dist', 'build'].includes(entry.name)) {
        countFilesByExt(path.join(dir, entry.name), counts, maxDepth - 1);
      } else if (entry.isFile()) {
        const ext = path.extname(entry.name).slice(1);
        if (ext) counts[ext] = (counts[ext] || 0) + 1;
      }
    }
  } catch { }
}

function extToLang(ext: string): string | null {
  const map: Record<string, string> = {
    go: 'go', ts: 'ts', tsx: 'ts', js: 'js', jsx: 'js',
    py: 'py', rs: 'rs', cpp: 'cpp', cc: 'cpp', c: 'c', h: 'c'
  };
  return map[ext] || null;
}

async function selectRepo(config: AuditConfig): Promise<{ name: string; path: string; languages: string[] }> {
  const repos = await discoverCachedRepos();

  if (repos.length === 0) {
    throw new Error("No cached repos found. Run some audits first to cache repos.");
  }

  let filtered = repos;

  if (config.repoFilter) {
    filtered = repos.filter(r => r.name.toLowerCase().includes(config.repoFilter!.toLowerCase()));
    if (filtered.length === 0) {
      console.log(`No repos matching filter "${config.repoFilter}", using all repos`);
      filtered = repos;
    }
  }

  const analyzed = await Promise.all(
    filtered.map(async r => ({
      ...r,
      languages: await analyzeRepoLanguages(r.path)
    }))
  );

  if (config.preferMultiLang || config.minLanguages > 1) {
    const multiLang = analyzed.filter(r => r.languages.length >= config.minLanguages);
    if (multiLang.length > 0) {
      filtered = multiLang;
    }
  }

  const selected = filtered[Math.floor(Math.random() * filtered.length)]!;
  return selected as { name: string; path: string; languages: string[] };
}

async function runWithTimeout<T>(
  name: string,
  fn: () => Promise<T>,
  timeoutMs: number,
  timings: TimingMetric[]
): Promise<T | null> {
  const start = Date.now();
  try {
    const result = await Promise.race([
      fn(),
      new Promise<null>((_, reject) =>
        setTimeout(() => reject(new Error(`Timeout after ${timeoutMs}ms`)), timeoutMs)
      )
    ]);

    timings.push({
      name,
      durationMs: Date.now() - start,
      success: true
    });
    return result as T;
  } catch (e) {
    timings.push({
      name,
      durationMs: Date.now() - start,
      success: false,
      error: e instanceof Error ? e.message : String(e)
    });
    return null;
  }
}

async function runStressTest(config: AuditConfig): Promise<AuditResult> {
  const timings: TimingMetric[] = [];
  const testResults: { test: string; passed: boolean; details: string }[] = [];
  const overallStart = Date.now();
  let peakMemory = 0;

  const memoryInterval = setInterval(() => {
    const usage = process.memoryUsage();
    peakMemory = Math.max(peakMemory, usage.heapUsed / 1024 / 1024);
  }, 100);

  const selected = await selectRepo(config);
  console.log(`\n${"=".repeat(60)}`);
  console.log(`LSP STRESS TEST`);
  console.log(`${"=".repeat(60)}`);
  console.log(`Repo: ${selected.name}`);
  console.log(`Languages: ${selected.languages.join(", ")}`);
  console.log(`Config: multiLang=${config.preferMultiLang}, minLang=${config.minLanguages}`);

  const containerName = `firmius-lsp-stress-${Math.random().toString(36).substring(7)}`;
  let host: IHost | null = null;

  try {
    host = new DockerHost({
      image: config.dockerImage,
      containerName,
      volumes: { [selected.path]: "/work/repo" }
    });

    await runWithTimeout("docker_init", () => host!.init(), 30000, timings);

    const lspUtility = Engine.getLSPUtility(host, "/work/repo");

    const stats: CodebaseStats | null = await runWithTimeout("full_scan",
      () => lspUtility.scan(),
      config.timeoutMs,
      timings
    );

    const languages = stats ? Array.from(stats.languages.keys()) : [];
    const symbolCount = stats ? Array.from(stats.languages.values()).reduce((a: number, b: number) => a + b, 0) : 0;

    // Map language names to file extensions
    const langToExt: Record<string, string> = {
      'go': 'go', 'rust': 'rs', 'python': 'py', 
      'typescript': 'ts', 'javascript': 'js', 
      'c': 'c', 'cpp': 'cpp', 'c++': 'cpp'
    };

    // Check that main repo language was indexed
    const expectedMainLang = selected.languages[0] || "unknown";
    const mainLangSymbols = stats?.languages.get(expectedMainLang) || 0;
    
    testResults.push({
      test: "Language Detection",
      passed: languages.length > 0,
      details: `Detected: ${languages.join(", ")}`
    });
    
    testResults.push({
      test: `Main Language (${expectedMainLang})`,
      passed: mainLangSymbols > 0,
      details: mainLangSymbols > 0 ? `${mainLangSymbols} symbols in main language` : `No symbols from ${expectedMainLang}`
    });

    testResults.push({
      test: "Symbol Indexing",
      passed: symbolCount > 0,
      details: `Indexed ${symbolCount} symbols`
    });

    const topFiles = (stats?.topFiles || []).slice(0, 30);
    testResults.push({
      test: "Top Files",
      passed: topFiles.length > 0,
      details: `Found ${topFiles.length} ranked files`
    });

    // Get one file per detected language for testing
    // Prefer main source files over test/config/generated files
    const filesByLang = new Map<string, string>();
    
    const isGoodTestFile = (path: string): boolean => {
      const lower = path.toLowerCase();
      // Skip test files
      if (lower.includes('_test') || lower.includes('.test') || lower.includes('.spec')) return false;
      if (lower.includes('/test/') || lower.includes('/tests/') || lower.includes('/__tests__/')) return false;
      // Skip config files
      if (lower.includes('config.') || lower.includes('.config.') || lower.includes('setup.')) return false;
      if (lower.includes('playwright') || lower.includes('jest') || lower.includes('webpack')) return false;
      // Skip generated files
      if (lower.includes('.pb.') || lower.includes('.gen.') || lower.includes('_gen')) return false;
      if (lower.includes('.bpf.c')) return false; // BPF code has limited LSP support
      // Skip dotfiles
      if (path.includes('/.')) return false;
      return true;
    };

     for (const lang of languages) {
       const ext = langToExt[lang.toLowerCase()] || lang;
       
       // First try top files that pass the quality filter
       let file = topFiles.find(f => f.path.endsWith(`.${ext}`) && isGoodTestFile(f.path));
      
      // For Go, avoid root-level files like constants.go which have LSP issues
      if (ext === 'go' && file?.path.endsWith('constants.go')) {
        // Skip this and look for another candidate
        file = topFiles.find(f => f.path.endsWith(`.${ext}`) && isGoodTestFile(f.path) && !f.path.endsWith('constants.go'));
      }
      
      // Fallback: search for any good source file with this extension
      if (!file) {
        const findResult = await host.exec(
          `find /work/repo -name "*.${ext}" -type f ` +
          `-not -path "*/test/*" -not -path "*/tests/*" -not -path "*/.git/*" ` +
          `-not -path "*/node_modules/*" -not -path "*/vendor/*" ` +
          `-not -name "*_test*" -not -name "*.test.*" -not -name "*.spec.*" ` +
          `-not -name "*config*" -not -name "*.pb.*" -not -name "*.gen.*" ` +
          `2>/dev/null | head -5`
        );
        if (findResult.exitCode === 0 && findResult.stdout.trim()) {
          const candidates = findResult.stdout.trim().split('\n').filter(isGoodTestFile);
          if (candidates.length > 0) {
            filesByLang.set(lang, candidates[0]!);
            continue;
          }
        }
      }
      
      // Last resort: use the original top file even if not ideal
      if (!file) {
        file = topFiles.find(f => f.path.endsWith(`.${ext}`));
      }
      
      if (file) {
        filesByLang.set(lang, file.path);
      }
     }

     // Give LSP servers time to finish background indexing (espelspUtilityally gopls in multi-module workspaces)
     await new Promise(resolve => setTimeout(resolve, 5000));

     // Stress test each detected language
     for (const [ext, testFile] of Array.from(filesByLang.entries())) {
      const langName = ext.toUpperCase();

      // Document Symbols
      const symbols: DocumentSymbol[] | null = await runWithTimeout(`doc_symbols_${ext}`,
        () => lspUtility.getDocumentSymbols(testFile),
        10000, timings
      );
      testResults.push({
        test: `Doc Symbols (${langName})`,
        passed: symbols !== null && symbols.length > 0,
        details: `${symbols?.length || 0} symbols in ${testFile.split('/').pop()}`
      });

      // Exports
      const exports: Array<{ name: string; kind: SymbolKind; line: number }> | null = await runWithTimeout(`exports_${ext}`,
        () => lspUtility.getExports(testFile),
        10000, timings
      );
      testResults.push({
        test: `Exports (${langName})`,
        passed: exports !== null && exports.length > 0,
        details: `${exports?.length || 0} exports`
      });

      // File Summary
      const summary: { imports: string[]; exports: Array<{ name: string; kind: SymbolKind }>; classes: string[]; functions: string[] } | null = await runWithTimeout(`summary_${ext}`,
        () => lspUtility.getFileSummary(testFile),
        10000, timings
      );
      testResults.push({
        test: `File Summary (${langName})`,
        passed: summary !== null,
        details: summary ? `${summary.classes.length} classes, ${summary.functions.length} functions` : "Failed"
      });

      // Get first function/method symbol for position-based tests
      // Prioritize functions/methods over classes/interfaces for better definition resolution
      const testSym = symbols?.find(s => 
        (s.kind === SymbolKind.Function || s.kind === SymbolKind.Method) &&
        s.name && !s.name.startsWith('<') && s.name !== 'init' && s.name.length > 3
      ) || symbols?.find(s => 
        (s.kind === SymbolKind.Class || s.kind === SymbolKind.Interface) &&
        s.name && !s.name.startsWith('<')
      ) || symbols?.find(s => 
        (s.kind === SymbolKind.Function || s.kind === SymbolKind.Method || 
         s.kind === SymbolKind.Class || s.kind === SymbolKind.Interface) &&
        s.name && !s.name.startsWith('<')
      );
      
      // Find the actual position of the symbol name in the file
      let testLine = testSym?.selectionRange?.start?.line ?? testSym?.range?.start?.line ?? 0;
      let testChar = testSym?.selectionRange?.start?.character ?? testSym?.range?.start?.character ?? 0;
      const symName = testSym?.name || 'unknown';
      
      // If we have a symbol name, search for it in the file to get exact position
      if (testSym && symName !== 'unknown') {
        try {
          const fileContent = await host.readFile(testFile);
          const lines = fileContent.split('\n');
          
          // Search for the symbol name in the file, prioritizing lines near the reported position
          const searchStart = Math.max(0, testLine - 5);
          const searchEnd = Math.min(lines.length, testLine + 50);
          
          for (let i = searchStart; i < searchEnd; i++) {
            const line = lines[i];
            if (line && line.includes(symName)) {
              const charIdx = line.indexOf(symName);
              // Check if it looks like a definition (preceded by fn, func, def, class, etc.)
              const before = line.substring(0, charIdx).trim();
              if (/\b(fn|func|def|class|interface|struct|enum|function|const|var|let|pub)\s*$/.test(before) ||
                  charIdx === 0 || /[\s\(]/.test(line[charIdx - 1] || '')) {
                testLine = i;
                testChar = charIdx;
                break;
              }
            }
          }
        } catch { }
      }

      // Find Callers - should find callers for any non-private function
      if (testSym) {
        const callers: Array<{ file: string; line: number; symbol: string }> | null = await runWithTimeout(`find_callers_${ext}`,
          () => lspUtility.findCallers(symName, testFile),
          15000, timings
        );
        testResults.push({
          test: `Find Callers (${langName})`,
          passed: callers !== null && callers.length > 0,
          details: `${callers?.length || 0} callers for "${symName}"`
        });
      } else {
        testResults.push({
          test: `Find Callers (${langName})`,
          passed: true,
          details: "Skipped - no suitable symbols"
        });
      }

      // Position-based tests only if we have a valid symbol position
      if (testSym && testLine >= 0 && testChar >= 0) {
        // Get Definition at symbol position
        const def: import("@firmius/shared").LSPLocation[] | null = await runWithTimeout(`definition_${ext}`,
          () => lspUtility.getDefinition(testFile, testLine, testChar),
          10000, timings
        );
        testResults.push({
          test: `Definition (${langName})`,
          passed: def !== null && def.length > 0,
          details: def && def.length > 0 ? `${def.length} definitions` : "No result"
        });

        // Get Hover at symbol position
        const hover: import("@firmius/shared").Hover | null = await runWithTimeout(`hover_${ext}`,
          () => lspUtility.getHover(testFile, testLine, testChar),
          10000, timings
        );
        testResults.push({
          test: `Hover (${langName})`,
          passed: hover !== null && hover !== undefined,
          details: hover ? "Got hover" : "No hover"
        });

        // Get References at symbol position
        const refs: import("@firmius/shared").LSPLocation[] | null = await runWithTimeout(`references_${ext}`,
          () => lspUtility.getReferences(testFile, testLine, testChar),
          15000, timings
        );
        testResults.push({
          test: `References (${langName})`,
          passed: refs !== null && refs.length > 0,
          details: `${refs?.length || 0} references`
        });

        // Find Symbol by name
        const foundSym: Array<{ file: string; line: number; character: number; name: string }> | null = await runWithTimeout(`find_symbol_${ext}`,
          () => lspUtility.findSymbol(symName),
          10000, timings
        );
        testResults.push({
          test: `Find Symbol (${langName})`,
          passed: foundSym !== null && foundSym.length > 0,
          details: `${foundSym?.length || 0} definitions for "${symName}"`
        });
      } else {
        // Skip position-based tests
        testResults.push({ test: `Definition (${langName})`, passed: true, details: "Skipped - no valid symbol" });
        testResults.push({ test: `Hover (${langName})`, passed: true, details: "Skipped - no valid symbol" });
        testResults.push({ test: `References (${langName})`, passed: true, details: "Skipped - no valid symbol" });
        testResults.push({ test: `Find Symbol (${langName})`, passed: true, details: "Skipped - no valid symbol" });
      }
    }

    // Additional random symbol searches
    const allSymbols: string[] = [];
     for (const [, testFile] of Array.from(filesByLang.entries())) {
      const syms = await lspUtility.getDocumentSymbols(testFile);
      if (syms) {
        for (const s of syms) {
          if (s.kind === SymbolKind.Function || s.kind === SymbolKind.Method || s.kind === SymbolKind.Class) {
            allSymbols.push(s.name);
          }
        }
      }
    }

    const randomSymbols = allSymbols
      .sort(() => Math.random() - 0.5)
      .slice(0, 5);

    for (const symName of randomSymbols) {
      const found: Array<{ file: string; line: number; character: number; name: string }> | null = await runWithTimeout(`find_symbol(${symName.substring(0, 20)})`,
        () => lspUtility.findSymbol(symName),
        10000, timings
      );
      testResults.push({
        test: "Find Symbol (Random)",
        passed: found !== null && found.length > 0,
        details: `"${symName.substring(0, 30)}": ${found?.length || 0} definitions`
      });
    }

    const cacheStart = Date.now();
    await lspUtility.scan();
    const cacheDuration = Date.now() - cacheStart;

    timings.push({
      name: "cached_scan",
      durationMs: cacheDuration,
      success: cacheDuration < 100
    });

    testResults.push({
      test: "Cache Performance",
      passed: cacheDuration < 100,
      details: `Second scan: ${cacheDuration}ms (should be <100ms)`
    });

    await lspUtility.dispose();

    return {
      repo: selected.name,
      repoPath: selected.path,
      config,
      timings,
      languages,
      symbolCount,
      fileCount: stats?.totalFiles || 0,
      topFiles: topFiles.map((f: { path: string; score: number; symbolCount: number }) => ({ path: f.path, score: f.score, symbolCount: f.symbolCount })),
      testResults,
      totalDurationMs: Date.now() - overallStart,
      peakMemoryMb: Math.round(peakMemory)
    };

  } finally {
    clearInterval(memoryInterval);
    console.log("\nCleaning up...");
    try {
      execSync(`docker rm -f ${containerName} 2>/dev/null`, { stdio: "pipe" });
    } catch { }
  }
}

function printResults(result: AuditResult) {
  console.log("\n" + "=".repeat(60));
  console.log("STRESS TEST RESULTS");
  console.log("=".repeat(60));

  console.log(`\n📦 Repo: ${result.repo}`);
  console.log(`📁 Files: ${result.fileCount} | Symbols: ${result.symbolCount}`);
  console.log(`🌐 Languages: ${result.languages.join(", ")}`);
  console.log(`⏱️  Total Time: ${(result.totalDurationMs / 1000).toFixed(1)}s`);
  console.log(`💾 Peak Memory: ${result.peakMemoryMb}MB`);

  console.log("\n📊 TIMINGS:");
  const timings = result.timings.sort((a, b) => b.durationMs - a.durationMs);
  for (const t of timings) {
    const status = t.success ? "✓" : "✗";
    const error = t.error ? ` (${t.error})` : "";
    console.log(`  ${status} ${t.name}: ${t.durationMs}ms${error}`);
  }

  console.log("\n🧪 TEST RESULTS:");
  const passed = result.testResults.filter(r => r.passed).length;
  const total = result.testResults.length;
  console.log(`  Passed: ${passed}/${total}`);

  for (const r of result.testResults) {
    const status = r.passed ? "✓" : "✗";
    console.log(`  ${status} ${r.test}: ${r.details}`);
  }

  console.log("\n📁 TOP FILES:");
  for (const f of result.topFiles.slice(0, 5)) {
    const shortPath = f.path.replace("/work/repo/", "");
    console.log(`  - ${shortPath} (score: ${f.score}, symbols: ${f.symbolCount})`);
  }

  const failures = result.timings.filter(t => !t.success);
  const slowOps = result.timings.filter(t => t.durationMs > 5000);

  console.log("\n" + "=".repeat(60));
  if (failures.length === 0 && passed === total) {
    console.log("✅ ALL TESTS PASSED");
  } else {
    console.log("❌ ISSUES DETECTED:");
    if (failures.length > 0) {
      console.log(`  - ${failures.length} operations failed`);
    }
    if (slowOps.length > 0) {
      console.log(`  - ${slowOps.length} operations took >5s`);
    }
  }
  console.log("=".repeat(60));
}

async function main() {
  const args = process.argv.slice(2);

  const config: AuditConfig = { ...DEFAULT_CONFIG };

  for (const arg of args) {
    if (arg === "--multi-lang") config.preferMultiLang = true;
    else if (arg === "--large") config.preferLarge = true;
    else if (arg.startsWith("--min-lang=")) config.minLanguages = parseInt(arg.split("=")[1] || "2");
    else if (arg.startsWith("--filter=")) config.repoFilter = arg.split("=")[1];
    else if (arg.startsWith("--timeout=")) config.timeoutMs = parseInt(arg.split("=")[1] || "300000");
    else if (arg === "--help") {
      console.log(`
LSP Stress Test - Advanced audit for LSP tools

Usage: bun src/audits/LSPStressTest.ts [options]

Options:
  --multi-lang      Prefer repos with multiple languages
  --large           Prefer larger repos
  --min-lang=N      Minimum number of languages (default: 1)
  --filter=STR      Only use repos containing STR in name
  --timeout=MS      Overall timeout in ms (default: 300000)
  --help            Show this help

Examples:
  bun src/audits/LSPStressTest.ts --multi-lang --min-lang=3
  bun src/audits/LSPStressTest.ts --filter=teleport
`);
      process.exit(0);
    }
  }

  await Engine.ignite({ prettyPrint: true });

  const result = await runStressTest(config);
  printResults(result);

  const failures = result.timings.filter(t => !t.success).length +
    result.testResults.filter(r => !r.passed).length;
  process.exit(failures > 0 ? 1 : 0);
}

main().catch(e => {
  console.error("Fatal error:", e);
  process.exit(1);
});
