import resolve from "@rollup/plugin-node-resolve";
import typescript from "@rollup/plugin-typescript";
import type { RollupOptions } from "rollup";

const config: RollupOptions = {
  input: "src/main.ts",
  output: {
    file: "dist/assets/bundle.js",
    format: "iife",
    name: "FirmiusFrontend",
    sourcemap: true,
  },
  plugins: [
    resolve({ browser: true }),
    typescript({ tsconfig: "./tsconfig.json" }),
  ],
};

export default config;
