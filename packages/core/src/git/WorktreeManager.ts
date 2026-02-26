import { join } from "node:path";
import type { IHost } from "@firmius/shared/types";

export class WorktreeError extends Error {
	constructor(
		message: string,
		public details?: { worktreePath?: string; branchName?: string },
	) {
		super(message);
		this.name = "WorktreeError";
	}
}

interface WorktreeEntry {
	path: string;
	branch: string;
	head: string;
}

export class WorktreeManager {
	private repoRoot: string;
	private host: IHost;

	constructor(options: { repoRoot: string; host: IHost }) {
		this.repoRoot = options.repoRoot;
		this.host = options.host;
	}

	private async runGit(args: string[]): Promise<string> {
		const cmd = `git ${args.join(' ')}`;
		const result = await this.host.exec(cmd, { cwd: this.repoRoot });

		if (result.exitCode !== 0) {
			throw new WorktreeError(
				`git ${args.join(' ')} failed (exit ${result.exitCode}): ${result.stderr.trim() || result.stdout.trim()}`,
			);
		}

		return result.stdout;
	}

	async createWorktree(options: {
		name: string;
		branch: string;
		baseBranch?: string;
	}): Promise<{ path: string; branch: string }> {
		const { name, branch, baseBranch = "main" } = options;

		const worktreePath = join(this.repoRoot, ".firmius", "worktrees", name);
		const branchName = branch;

		await this.runGit(
			["worktree", "add", "-b", branchName, worktreePath, baseBranch],
		);

		return { path: worktreePath, branch: branchName };
	}

	async listWorktrees(): Promise<WorktreeEntry[]> {
		const stdout = await this.runGit(["worktree", "list", "--porcelain"]);
		return WorktreeManager.parseWorktreeOutput(stdout);
	}

	async removeWorktree(path: string, options?: { force?: boolean; deleteBranch?: boolean }): Promise<void> {
		const worktrees = await this.listWorktrees();
		const entry = worktrees.find((wt) => wt.path === path);
		const branchName = entry?.branch ?? "";

		const removeArgs = ["worktree", "remove", path];
		if (options?.force) {
			removeArgs.push("--force");
		}
		await this.runGit(removeArgs);

		if (options?.deleteBranch && branchName.length > 0) {
			try {
				await this.runGit(["branch", "-D", branchName]);
			} catch {
				// Best-effort
			}
		}
	}

	async mergeBranch(options: { sourceBranch: string; targetBranch: string; dryRun?: boolean }): Promise<{
		success: boolean;
		mergedCommits?: number;
		filesChanged?: string[];
		conflictFiles?: string[];
		message?: string;
	}> {
		const { sourceBranch, targetBranch, dryRun } = options;

		const originalBranch = (await this.runGit(["rev-parse", "--abbrev-ref", "HEAD"])).trim();

		let hasStash = false;
		try {
			const stashOutput = await this.runGit(["stash", "push", "-m", "firmius-merge-autostash"]);
			hasStash = !stashOutput.includes("No local changes");
		} catch {
			// No changes to stash
		}

		const restore = async () => {
			if (originalBranch !== targetBranch) {
				try {
					await this.runGit(["checkout", originalBranch]);
				} catch {
					// Best-effort restore
				}
			}
			if (hasStash) {
				try {
					await this.runGit(["stash", "pop"]);
				} catch {
					// Stash pop can fail
				}
			}
		};

		try {
			await this.runGit(["checkout", targetBranch]);

			if (dryRun) {
				try {
					await this.runGit(["merge", "--no-commit", "--no-ff", sourceBranch]);
				} catch {
					const conflictFiles = await WorktreeManager.getConflictFiles(this);
					try { await this.runGit(["merge", "--abort"]); } catch {}
					await restore();
					return {
						success: false,
						conflictFiles,
						message: `Dry run: merge would conflict on ${conflictFiles.length} file(s)`,
					};
				}

				const mergedCommits = await WorktreeManager.countMergeCommits(
					this,
					targetBranch,
					sourceBranch,
				);
				const filesChanged = await WorktreeManager.getChangedFilesFromMerge(this);

				try { await this.runGit(["merge", "--abort"]); } catch {}
				await restore();
				return {
					success: true,
					mergedCommits,
					filesChanged,
					message: `Dry run: merge would succeed with ${mergedCommits} commit(s)`,
				};
			}

			// Real merge
			try {
				await this.runGit(["merge", sourceBranch, "-m", `Merge ${sourceBranch} into ${targetBranch}`]);
			} catch {
				const conflictFiles = await WorktreeManager.getConflictFiles(this);
				try { await this.runGit(["merge", "--abort"]); } catch {}
				await restore();
				return {
					success: false,
					conflictFiles,
					message: `Merge conflict on ${conflictFiles.length} file(s)`,
				};
			}

			const mergedCommits = await WorktreeManager.countMergeCommits(
				this,
				targetBranch,
				sourceBranch,
			);
			const filesChanged = await WorktreeManager.getChangedFiles(this);

			await restore();
			return {
				success: true,
				mergedCommits,
				filesChanged,
				message: `Merged ${mergedCommits} commit(s) from ${sourceBranch} into ${targetBranch}`,
			};
		} catch (err) {
			await restore();
			throw err;
		}
	}

	async prune(): Promise<void> {
		await this.runGit(["worktree", "prune"]);
	}

	private static parseWorktreeOutput(output: string): WorktreeEntry[] {
		const entries: WorktreeEntry[] = [];
		const blocks = output.trim().split("\n\n");

		for (const block of blocks) {
			if (block.trim() === "") continue;

			let path = "";
			let head = "";
			let branch = "";

			const lines = block.trim().split("\n");
			for (const line of lines) {
				if (line.startsWith("worktree ")) {
					path = line.slice("worktree ".length);
				} else if (line.startsWith("HEAD ")) {
					head = line.slice("HEAD ".length);
				} else if (line.startsWith("branch ")) {
					const ref = line.slice("branch ".length);
					branch = ref.replace(/^refs\/heads\//, "");
				}
			}

			if (path.length > 0) {
				entries.push({ path, branch, head });
			}
		}

		return entries;
	}

	private static async getConflictFiles(manager: WorktreeManager): Promise<string[]> {
		try {
			const output = await manager.runGit(["diff", "--name-only", "--diff-filter=U"]);
			return output.trim().split("\n").filter((f) => f.length > 0);
		} catch {
			return [];
		}
	}

	private static async getChangedFiles(manager: WorktreeManager): Promise<string[]> {
		try {
			const output = await manager.runGit(["diff", "--name-only", "HEAD~1"]);
			return output.trim().split("\n").filter((f) => f.length > 0);
		} catch {
			return [];
		}
	}

	private static async getChangedFilesFromMerge(manager: WorktreeManager): Promise<string[]> {
		try {
			const output = await manager.runGit(["diff", "--cached", "--name-only"]);
			return output.trim().split("\n").filter((f) => f.length > 0);
		} catch {
			return [];
		}
	}

	private static async countMergeCommits(
		manager: WorktreeManager,
		targetBranch: string,
		sourceBranch: string,
	): Promise<number> {
		try {
			const output = await manager.runGit([
				"log",
				"--oneline",
				`${targetBranch}..${sourceBranch}`,
			]);
			const lines = output.trim().split("\n").filter((l) => l.length > 0);
			return lines.length;
		} catch {
			return 0;
		}
	}
}
