import { promises as fs } from 'fs';
import { join } from 'path';
import { homedir } from 'os';
import { DEFAULT_USER_CONFIG, type UserConfig } from './UserConfig';

class UserConfigManager {
  private static instance: UserConfigManager | null = null;

  private config: UserConfig = DEFAULT_USER_CONFIG;
  private loaded: boolean = false;

  private constructor() {}

  public static getInstance(): UserConfigManager {
    if (!UserConfigManager.instance) {
      UserConfigManager.instance = new UserConfigManager();
    }
    return UserConfigManager.instance;
  }

   public async load(): Promise<void> {
     if (this.loaded) return;
     try {
       const configPath = join(homedir(), '.firmius', 'config.json');
       await fs.access(configPath);
       const data = await fs.readFile(configPath, 'utf-8');
       this.config = JSON.parse(data);
     } catch {
       this.config = DEFAULT_USER_CONFIG;
       await this.save();
     }
     this.loaded = true;
   }

   public async refresh(): Promise<void> {
     this.loaded = false;
     await this.load();
   }

  public get(): UserConfig {
    return this.config;
  }

  public set(config: UserConfig): void {
    this.config = config;
  }

  public async save(): Promise<void> {
    const configDir = join(homedir(), '.firmius');
    await fs.mkdir(configDir, { recursive: true });
    const configPath = join(configDir, 'config.json');
    await fs.writeFile(configPath, JSON.stringify(this.config, null, 2));
  }
}

export default UserConfigManager;
