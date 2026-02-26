 import { randomUUID } from "node:crypto";
 import { EventEmitter } from "node:events";
 import type { HostProcessHandle } from "@firmius/shared";

 export class ProcessManager extends EventEmitter {
     private handles: Map<string, HostProcessHandle> = new Map();
     private outputs: Map<string, string[]> = new Map();
     private commands: Map<string, string> = new Map();

     register(handle: HostProcessHandle): string {
         const id = randomUUID();
         this.handles.set(id, handle);
         this.outputs.set(id, []); // initialize output buffer

         // Attach output listener
         handle.onOutput((data: string, src: "stdout" | "stderr") => {
           const buf = this.outputs.get(id) || [];
           buf.push(data);
           this.outputs.set(id, buf);
           this.emit('output', { id, data, src });
         });

         return id;
     }

     setCommand(id: string, command: string): void {
         this.commands.set(id, command);
     }

     getCommand(id: string): string {
         return this.commands.get(id) || '';
     }

     get(id: string): HostProcessHandle | undefined {
         return this.handles.get(id);
     }

     unregister(id: string) {
         this.handles.delete(id);
         // Optionally keep outputs for later retrieval; not deleting here.
     }

     list(): { id: string, pid: number, completed: boolean }[] {
         return Array.from(this.handles.entries()).map(([id, h]) => ({
             id,
             pid: h.pid,
             completed: h.completed
         }));
     }

     getOutput(id: string): string[] {
         return this.outputs.get(id) || [];
     }
 }
