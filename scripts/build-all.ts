#!/usr/bin/env bun

import { readFile, writeFile, mkdir, rm } from "node:fs/promises";
import { join } from "node:path";
import { spawn } from "node:child_process";

const ROOT = process.cwd();
const DIST = join(ROOT, "dist");
const WEB_OUT = join(ROOT, "packages/web/out");
const CLI_SRC = join(ROOT, "packages/cli/src");

async function log(msg: string) {
  console.log(`[build] ${msg}`);
}

async function exec(cmd: string, cwd: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = spawn("sh", ["-c", cmd], { cwd, stdio: "inherit" });
    child.on("close", (code: number | null) => (code === 0 ? resolve() : reject(new Error(`exit ${code}`))));
  });
}

async function compressWebStatic(): Promise<string> {
  log("Compressing web static files...");
  
  // Create tar.gz of web/out
  const tarPath = join(DIST, "web.tar.gz");
  
  // Remove old tar if exists
  try {
    await rm(tarPath, { force: true });
  } catch {}
  
  // Create tar.gz - web export goes to packages/web/out
  await exec(`tar -czf ${tarPath} -C packages/web/out .`, ROOT);
  
  // Read and convert to base64
  const tarData = await readFile(tarPath);
  return tarData.toString("base64");
}

async function buildAll() {
  log("Starting full build...");
  
  // 1. Ensure dist exists
  await mkdir(DIST, { recursive: true });
  
  // 2. Embed purposes (generate purposes.embedded.ts)
  log("Embedding purposes...");
  await exec("bun run scripts/embedPurposes.ts", ROOT);
  
  // 3. Build web (Next.js static export)
  log("Building web (next build)...");
  await exec("bun run next build", join(ROOT, "packages/web"));
  
  // 4. Compress and embed web static
  const webBase64 = await compressWebStatic();
  
  // 5. Generate embedded web file
  log("Generating embedded web static...");
  const webEmbedContent = `// Auto-generated - do not edit
export const EMBEDDED_WEB_STATIC = \`${webBase64}\`;
`;
  await writeFile(join(CLI_SRC, "web.embedded.ts"), webEmbedContent);
  
  // 6. Build CLI binary (includes all deps bundled)
  log("Building CLI binary...");
  await exec(
    "bun build src/index.tsx --compile --outfile dist/firmius",
    join(ROOT, "packages/cli")
  );
  
  // 7. Copy to root dist
  await exec("cp packages/cli/dist/firmius dist/firmius", ROOT);
  
  // 8. Make executable
  await exec("chmod +x dist/firmius", ROOT);
  
  log("✅ Build complete: dist/firmius");
}

buildAll().catch(e => {
  console.error("Build failed:", e);
  process.exit(1);
});