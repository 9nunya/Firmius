import type { IHost } from "@firmius/shared";

interface ProjectInitResult {
  initialized: boolean;
  wasEmpty: boolean;
  gitInitialized: boolean;
  firmiusCreated: boolean;
}

const FIRMUS_DIRS = ["phases", "context"];

async function pathExists(host: IHost, path: string): Promise<boolean> {
  try {
    await host.exec(`test -e "${path}"`);
    return true;
  } catch {
    return false;
  }
}

async function isDirectoryEmpty(host: IHost, cwd: string): Promise<boolean> {
  try {
    const result = await host.exec(`ls -A "${cwd}" 2>/dev/null | head -1`);
    return result.stdout.trim() === "";
  } catch {
    return true;
  }
}

async function hasGitRepo(host: IHost, cwd: string): Promise<boolean> {
  try {
    await host.exec(`git rev-parse --git-dir`, { cwd });
    return true;
  } catch {
    return false;
  }
}

async function hasFirmiusDir(host: IHost, cwd: string): Promise<boolean> {
  return pathExists(host, `${cwd}/.firmius`);
}

export async function ensureProject(
  host: IHost,
  cwd: string,
): Promise<ProjectInitResult> {
  const result: ProjectInitResult = {
    initialized: false,
    wasEmpty: false,
    gitInitialized: false,
    firmiusCreated: false,
  };

  const hasGit = await hasGitRepo(host, cwd);
  const hasFirmius = await hasFirmiusDir(host, cwd);

  if (!hasGit) {
    const isEmpty = await isDirectoryEmpty(host, cwd);
    result.wasEmpty = isEmpty;

    if (isEmpty) {
      await host.exec(`git init`, { cwd });
      await host.exec(`git config user.email "firmius@local"`, { cwd });
      await host.exec(`git config user.name "Firmius"`, { cwd });

      await host.exec(`touch "${cwd}/.gitkeep"`, { cwd });
      await host.exec(`git add .gitkeep`, { cwd });
      await host.exec(`git commit -m "Initial commit"`, { cwd });

      result.gitInitialized = true;
    }
  }

  if (!hasFirmius) {
    await host.exec(`mkdir -p "${cwd}/.firmius"`, { cwd });

    for (const dir of FIRMUS_DIRS) {
      await host.exec(`mkdir -p "${cwd}/.firmius/${dir}"`, { cwd });
    }

    await host.exec(`touch "${cwd}/.firmius/state.db"`, { cwd });

    const gitignore = `# Firmius internal files
state.db
*.db-journal
*.db-wal
phases/
context/
`;
    await host.exec(
      `cat > "${cwd}/.firmius/.gitignore" << 'EOF'
${gitignore}
EOF`,
      { cwd },
    );

    result.firmiusCreated = true;
  }

  result.initialized = result.gitInitialized || result.firmiusCreated;

  return result;
}

export async function ensureFirmiusStructure(
  host: IHost,
  cwd: string,
): Promise<void> {
  const firmiusPath = `${cwd}/.firmius`;

  if (!(await pathExists(host, firmiusPath))) {
    await host.exec(`mkdir -p "${firmiusPath}"`, { cwd });
  }

  for (const dir of FIRMUS_DIRS) {
    const dirPath = `${firmiusPath}/${dir}`;
    if (!(await pathExists(host, dirPath))) {
      await host.exec(`mkdir -p "${dirPath}"`, { cwd });
    }
  }
}

export async function isFirmiusProject(
  host: IHost,
  cwd: string,
): Promise<boolean> {
  return hasFirmiusDir(host, cwd);
}
