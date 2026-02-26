// Main facade
export { LSPUtility } from './LSPUtility';
export type { CodebaseStats, StructureSummary } from './LSPUtility';

// Protocol
export { LSPProtocol, ConnectionState } from './LSPProtocol';

// Server Registry
export { LSPServerRegistry } from './LSPServerRegistry';
export type { LSPServerConfig } from './LSPServerRegistry';

// Symbol Indexer
export { SymbolIndexer, SymbolKindNames } from './SymbolIndexer';
export type { SymbolInfo, ISymbolIndexerClient, ISymbolIndexerRegistry, ISymbolIndexerHost } from './SymbolIndexer';

// Reference Graph
export { ReferenceGraph } from './ReferenceGraph';
export type { TopFileResult, ReferenceSummaryEntry, ReferenceGraphData } from './ReferenceGraph';

// Semantic Operations
export { SemanticOperations } from './SemanticOperations';
export type { ISemanticOperationsRegistry, ISemanticOperationsHost } from './SemanticOperations';

// File Scanner
export { FileScanner } from './FileScanner';
export type { EntryPoint, FileStats, IFileScannerHost, IFileScannerRegistry } from './FileScanner';

// Language Detector
export { LanguageDetector } from './LanguageDetector';
export type { DetectionResult, IDetectorHost, IDetectorRegistry } from './LanguageDetector';

// Language Handler
export { LanguageHandler } from './LanguageHandler';
export type { LuauWorkspaceInfo, RojoManifest, IHandlerHost, IHandlerRegistry } from './LanguageHandler';

// Default export
export { LSPUtility as default } from './LSPUtility';
