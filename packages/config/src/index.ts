export const typescript = {
  compilerOptions: {
    target: "ES2022",
    module: "ESNext",
    lib: ["ES2022"],
    moduleResolution: "bundler",
    allowImportingTsExtensions: true,
    noEmit: true,
    verbatimModuleSyntax: true,
    isolatedModules: true,
    esModuleInterop: true,
    skipLibCheck: true,
    forceConsistentCasingInFileNames: true,
    resolveJsonModule: true,
    allowSyntheticDefaultImports: true,
    strict: true,
    noUnusedLocals: true,
    noUnusedParameters: true,
    noFallthroughCasesInSwitch: true,
  },
};

export const react = {
  compilerOptions: {
   jsx: "react-jsx",
  },
};
