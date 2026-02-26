/** @jsxImportSource @opentui/react */
import { createCliRenderer } from "@opentui/core";
import { createRoot } from "@opentui/react";
export { App } from "./App";
import { App } from "./App";

async function main() {
  const renderer = await createCliRenderer();
  const root = createRoot(renderer);
  root.render(<App />);

  // Handle cleanup on exit
  process.on("SIGINT", () => {
    renderer.destroy();
    process.exit(0);
  });

  process.on("SIGTERM", () => {
    renderer.destroy();
    process.exit(0);
  });
}

main().catch(console.error);
