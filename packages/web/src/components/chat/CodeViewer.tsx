import React from 'react';
import { MarkdownContent } from './MarkdownContent';

const extensionMap: Record<string, string> = {
  ts: 'typescript',
  tsx: 'typescript',
  js: 'javascript',
  jsx: 'javascript',
  mjs: 'javascript',
  cjs: 'javascript',
  py: 'python',
  pyw: 'python',
  rb: 'ruby',
  go: 'go',
  rs: 'rust',
  java: 'java',
  kt: 'kotlin',
  kts: 'kotlin',
  swift: 'swift',
  m: 'objective-c',
  h: 'c',
  hpp: 'cpp',
  c: 'c',
  cpp: 'cpp',
  cc: 'cpp',
  cxx: 'cpp',
  cs: 'csharp',
  csx: 'csharp',
  css: 'css',
  scss: 'scss',
  sass: 'scss',
  less: 'less',
  html: 'html',
  htm: 'html',
  xml: 'xml',
  json: 'json',
  yaml: 'yaml',
  yml: 'yaml',
  toml: 'toml',
  md: 'markdown',
  sql: 'sql',
  sh: 'bash',
  bash: 'bash',
  zsh: 'bash',
  fish: 'fish',
  ps1: 'powershell',
  psm1: 'powershell',
  psd1: 'powershell',
  dockerfile: 'dockerfile',
  php: 'php',
  r: 'r',
  lua: 'lua',
  pl: 'perl',
  pm: 'perl',
  perl: 'perl',
  htaccess: 'apache',
  conf: 'conf',
  config: 'conf',
  ini: 'ini',
  env: 'dotenv',
  lock: 'json',
  graphql: 'graphql',
  gql: 'graphql',
  vue: 'vue',
  svelte: 'svelte',
  astro: 'astro',
  sol: 'solidity',
  vy: 'vyper',
  move: 'move',
  cadence: 'cadence',
  rfc: 'rfc',
  sty: 'latex',
  tex: 'latex',
  bib: 'bibtex',
  clj: 'clojure',
  cljs: 'clojure',
  cljc: 'clojure',
  edn: 'clojure',
  el: 'elisp',
  lisp: 'lisp',
  scm: 'scheme',
  ss: 'scheme',
  rkt: 'racket',
  dy: 'd',
  d: 'd',
  f: 'fortran',
  f90: 'fortran',
  f95: 'fortran',
  for: 'fortran',
  v: 'verilog',
  sv: 'systemverilog',
  vhd: 'vhdl',
  vhdl: 'vhdl',
  tcl: 'tcl',
  tk: 'tcl',
  wish: 'tcl',
  adb: 'ada',
  ads: 'ada',
  ada: 'ada',
  cob: 'cobol',
  cbl: 'cobol',
  jl: 'julia',
  pas: 'pascal',
  dpr: 'delphi',
  dproj: 'delphi',
  res: 'resource',
  rc: 'rc',
  reg: 'reg',
  bat: 'batch',
  cmd: 'batch',
  awk: 'awk',
  gawk: 'awk',
  nawk: 'awk',
  sed: 'sed',
  make: 'makefile',
  mk: 'makefile',
  mkfile: 'makefile',
  cmake: 'cmake',
  roff: 'roff',
  man: 'man',
  me: 'troff',
  ms: 'troff',
  nsis: 'nsis',
  iss: 'inno',
  installscript: 'installscript',
  is: 'installshield',
  gd: 'gd',
  bas: 'basic',
  vb: 'vbnet',
  vbs: 'vbscript',
  as: 'actionscript',
  asc: 'actionscript',
  mxml: 'mxml',
  actionscript: 'actionscript',
  '3': 'terraform',
  tf: 'terraform',
  hcl: 'hcl',
  nomad: 'nomad',
  pkr: 'packer',
  pkrv: 'packer',
  jinja: 'jinja',
  j2: 'jinja',
  jinja2: 'jinja',
  mustache: 'mustache',
  handlebars: 'handlebars',
  hbs: 'handlebars',
  dfs: 'dfs',
  dfss: 'dfs',
}

interface CodeViewerProps {
  code: string;
  filename?: string;
  language?: string;
  maxHeight?: string;
}

export function CodeViewer({
  code,
  filename,
  language: explicitLanguage,
  maxHeight = '500px',
}: CodeViewerProps) {
  const lang = React.useMemo(() => {
    if (explicitLanguage) return explicitLanguage;
    if (filename) {
      const ext = filename.split('.').pop()?.toLowerCase() || '';
      return extensionMap[ext] || 'text';
    }
    return 'text';
  }, [explicitLanguage, filename]);

  // Wrap in markdown code fence
  const content = `\`\`\`${lang}\n${code}\n\`\`\``;

  return (
    <div className="rounded-lg border border-border/50 bg-black/5 dark:bg-black/20 overflow-auto" style={{ maxHeight }}>
      <div className="p-4">
        <MarkdownContent content={content} />
      </div>
    </div>
  );
}

export default CodeViewer;
