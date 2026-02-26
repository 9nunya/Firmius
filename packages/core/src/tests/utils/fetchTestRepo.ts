import path from "node:path";
import os from "node:os";
import fs from "node:fs";
import { execSync } from "node:child_process";

interface SWEBenchInstance {
  instance_id: string;
  repo: string;
  base_commit: string;
  problem_statement: string;
  hints_text?: string;
  requirements?: string;
  interface?: string;
  FAIL_TO_PASS: string;
  PASS_TO_PASS: string;
  repo_language?: string;
}

const REPO_CACHE_DIR = process.env.SWE_REPO_CACHE || path.join(os.homedir(), ".firmius/swebench-temp-repos");

/**
 * Fetches a random SWE-Bench Pro instance from Hugging Face dataset.
 * @returns Promise resolving to a random SWE-Bench instance
 * @throws Error if dataset fetch fails
 */
export async function fetchRandomSWEBenchRepo(): Promise<SWEBenchInstance> {
  const datasetName = "ScaleAI%2FSWE-bench_Pro";

  console.log(`Fetching random SWE-bench Pro instance...`);

  try {
    const infoRes = await fetch(
      `https://datasets-server.huggingface.co/info?dataset=${datasetName}&config=default`
    );
    if (!infoRes.ok) {
      throw new Error(`Failed to fetch dataset info: ${infoRes.status}`);
    }
    const info = await infoRes.json() as { dataset_info: { splits: { name: string; num_examples: number }[] } };

    const split = info.dataset_info.splits[0];
    const rowCount = split?.num_examples || 100;

    const res = await fetch(
      `https://datasets-server.huggingface.co/rows?dataset=${datasetName}&config=default&split=test&offset=0&length=${rowCount}`
    );
    if (!res.ok) {
      throw new Error(`Failed to fetch dataset: ${res.status}`);
    }
    const data = await res.json() as { rows: { row: any }[] };
    const instances: SWEBenchInstance[] = data.rows.map((r) => ({ ...r.row }));
    const randomIndex = Math.floor(Math.random() * instances.length);
    const instance = instances[randomIndex]!;
    console.log(
      `Selected instance: ${instance.instance_id} (${instance.repo}) [${instance.repo_language || "python"}]`
    );
    return instance;
  } catch (error) {
    console.error("Failed to fetch SWE-Bench instance:", error);
    throw error;
  }
}

/**
 * Ensures a repository is cached locally, cloning it if necessary.
 * @param repo - The GitHub repository in "owner/repo" format
 * @returns Promise resolving to the local cache path
 * @throws Error if cloning fails
 */
export async function ensureRepoCached(repo: string): Promise<string> {
   const cacheDir = REPO_CACHE_DIR;

   if (!fs.existsSync(cacheDir)) {
     fs.mkdirSync(cacheDir, { recursive: true });
   }

  const repoCachePath = path.join(cacheDir, repo);

   if (fs.existsSync(repoCachePath)) {
    console.log(`Using cached repo: ${repoCachePath}`);
    return repoCachePath;
  }

   console.log(`Cloning and caching repo: ${repoCachePath}`);

   try {
    execSync(`git clone https://github.com/${repo}.git ${repoCachePath}`, {
      stdio: "inherit",
    });
  } catch (e: any) {
    execSync(`rm -rf ${repoCachePath}`, { stdio: "inherit" });
    throw new Error(`Failed to clone/cache repo: ${e.message}`);
  }

  return repoCachePath;
}

/**
 * Returns the configured repository cache directory.
 * @returns The cache directory path
 */
export function getRepoCacheDir(): string {
  return REPO_CACHE_DIR;
}
