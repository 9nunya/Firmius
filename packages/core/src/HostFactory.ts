import type { HostConfig, IHost, DockerHostOptions } from "@firmius/shared/types";
import { HostType } from "@firmius/shared/types";
import { LocalHost } from "./hosts/Local";
import { DockerHost } from "./hosts/DockerHost";
import { RemoteSSHHost } from "./hosts/RemoteSSHHost";

export class HostFactory {
  static async create(config: HostConfig): Promise<IHost> {
    switch (config.type) {
      case HostType.Local:
        return new LocalHost();
      case HostType.Docker: {
        const opts = config as unknown as { options?: DockerHostOptions };
        const dockerOptions: DockerHostOptions = opts.options || { image: "firmius-sandbox:latest", containerName: `firmius-${Date.now()}` };
        if (!dockerOptions.image) {
          dockerOptions.image = "firmius-sandbox:latest";
        }
        if (!dockerOptions.containerName) {
          dockerOptions.containerName = `firmius-${Date.now()}`;
        }
        return new DockerHost(dockerOptions);
      }
      case HostType.RemoteSSH: {
        const opts = config as unknown as { options?: Record<string, unknown> };
        const sshOptions = opts.options || {};
        if (sshOptions.privateKeyPath) {
          sshOptions.privateKey = await Bun.file(sshOptions.privateKeyPath as string).text();
        }
        return new RemoteSSHHost(sshOptions as any);
      }
      default:
        const exhaustiveCheck: never = config;
        throw new Error(`Unknown host type: ${exhaustiveCheck}`);
    }
  }
}