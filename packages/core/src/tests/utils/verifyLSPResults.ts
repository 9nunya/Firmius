import type { DocumentSymbol, SymbolInformation, LSPDiagnostic, LSPRange } from "@firmius/shared";

interface Location {
  uri: string;
  range: LSPRange;
}

interface LocationLink {
  targetUri: string;
  targetRange: LSPRange;
}

interface TopFile {
  uri: string;
  score: number;
  symbolCount: number;
  referenceCount: number;
}

interface CallHierarchyNode {
  name: string;
  uri: string;
  range: {
    start: { line: number; character: number };
    end: { line: number; character: number };
  };
  children?: CallHierarchyNode[];
}

interface ReferencesGraph {
  nodes: Array<{
    id: string;
    name: string;
    uri: string;
    range: {
      start: { line: number; character: number };
      end: { line: number; character: number };
    };
  }>;
  edges: Array<{
    from: string;
    to: string;
    type: string;
  }>;
}

interface VerificationResult {
  valid: boolean;
  errors: string[];
}

/**
 * Verifies that symbols array is valid and contains required properties.
 * @param symbols - Array of symbols from LSP
 * @returns Verification result with any errors
 */
export function verifySymbols(
  symbols: (DocumentSymbol | SymbolInformation)[]
): VerificationResult {
  const errors: string[] = [];

  if (!Array.isArray(symbols)) {
    errors.push("Symbols must be an array");
    return { valid: false, errors };
  }

  for (let i = 0; i < symbols.length; i++) {
    const symbol = symbols[i];
    if (!symbol) {
      errors.push(`Symbol at index ${i} is null or undefined`);
      continue;
    }

    if (!("name" in symbol)) {
      errors.push(`Symbol at index ${i} missing required property: name`);
    }

    if (!("kind" in symbol)) {
      errors.push(`Symbol at index ${i} missing required property: kind`);
    }
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Verifies that locations array is valid and contains required properties.
 * @param locations - Array of locations from LSP
 * @returns Verification result with any errors
 */
export function verifyLocations(
  locations: (Location | LocationLink)[]
): VerificationResult {
  const errors: string[] = [];

  if (!Array.isArray(locations)) {
    errors.push("Locations must be an array");
    return { valid: false, errors };
  }

  for (let i = 0; i < locations.length; i++) {
    const location = locations[i];
    if (!location) {
      errors.push(`Location at index ${i} is null or undefined`);
      continue;
    }

    // LocationLink has targetUri, Location has uri
    const uri = "targetUri" in location ? location.targetUri : location.uri;
    if (!uri) {
      errors.push(`Location at index ${i} missing required property: uri`);
    }

    // LocationLink has targetRange, Location has range
    const range =
      "targetRange" in location ? location.targetRange : location.range;
    if (!range) {
      errors.push(`Location at index ${i} missing required property: range`);
    }
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Verifies that diagnostics array is valid with proper severity levels.
 * @param diagnostics - Array of diagnostics from LSP
 * @returns Verification result with any errors
 */
export function verifyDiagnostics(diagnostics: LSPDiagnostic[]): VerificationResult {
  const errors: string[] = [];

  if (!Array.isArray(diagnostics)) {
    errors.push("Diagnostics must be an array");
    return { valid: false, errors };
  }

  const validSeverities = [1, 2, 3, 4]; // Error, Warning, Information, Hint

  for (let i = 0; i < diagnostics.length; i++) {
    const diagnostic = diagnostics[i];
    if (!diagnostic) {
      errors.push(`Diagnostic at index ${i} is null or undefined`);
      continue;
    }

    if (!("message" in diagnostic)) {
      errors.push(`Diagnostic at index ${i} missing required property: message`);
    }

    if (!("range" in diagnostic)) {
      errors.push(`Diagnostic at index ${i} missing required property: range`);
    }

    if (
      "severity" in diagnostic &&
      diagnostic.severity !== undefined &&
      !validSeverities.includes(diagnostic.severity)
    ) {
      errors.push(
        `Diagnostic at index ${i} has invalid severity: ${diagnostic.severity}`
      );
    }
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Verifies that top files array has valid scores and counts.
 * @param files - Array of top files from LSP
 * @returns Verification result with any errors
 */
export function verifyTopFiles(files: TopFile[]): VerificationResult {
  const errors: string[] = [];

  if (!Array.isArray(files)) {
    errors.push("Files must be an array");
    return { valid: false, errors };
  }

  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    if (!file) {
      errors.push(`File at index ${i} is null or undefined`);
      continue;
    }

    if (!("uri" in file)) {
      errors.push(`File at index ${i} missing required property: uri`);
    }

    if (!("score" in file)) {
      errors.push(`File at index ${i} missing required property: score`);
    } else if (typeof file.score !== "number") {
      errors.push(`File at index ${i} has non-numeric score: ${file.score}`);
    } else if (file.score < 0) {
      errors.push(`File at index ${i} has negative score: ${file.score}`);
    }

    if (!("symbolCount" in file)) {
      errors.push(`File at index ${i} missing required property: symbolCount`);
    } else if (typeof file.symbolCount !== "number") {
      errors.push(
        `File at index ${i} has non-numeric symbolCount: ${file.symbolCount}`
      );
    } else if (file.symbolCount < 0) {
      errors.push(
        `File at index ${i} has negative symbolCount: ${file.symbolCount}`
      );
    }

    if (!("referenceCount" in file)) {
      errors.push(
        `File at index ${i} missing required property: referenceCount`
      );
    } else if (typeof file.referenceCount !== "number") {
      errors.push(
        `File at index ${i} has non-numeric referenceCount: ${file.referenceCount}`
      );
    } else if (file.referenceCount < 0) {
      errors.push(
        `File at index ${i} has negative referenceCount: ${file.referenceCount}`
      );
    }
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Verifies that call hierarchy nodes form a valid tree structure.
 * @param nodes - Array of call hierarchy nodes
 * @returns Verification result with any errors
 */
export function verifyCallHierarchy(nodes: CallHierarchyNode[]): VerificationResult {
  const errors: string[] = [];

  if (!Array.isArray(nodes)) {
    errors.push("Call hierarchy nodes must be an array");
    return { valid: false, errors };
  }

  function verifyNode(node: CallHierarchyNode, depth: number, index: string): void {
    if (!node) {
      errors.push(`Node at ${index} is null or undefined`);
      return;
    }

    if (!("name" in node)) {
      errors.push(`Node at ${index} missing required property: name`);
    }

    if (!("uri" in node)) {
      errors.push(`Node at ${index} missing required property: uri`);
    }

    if (!("range" in node)) {
      errors.push(`Node at ${index} missing required property: range`);
    } else {
      const range = node.range;
      if (!range.start || typeof range.start.line !== "number") {
        errors.push(`Node at ${index} has invalid range.start`);
      }
      if (!range.end || typeof range.end.line !== "number") {
        errors.push(`Node at ${index} has invalid range.end`);
      }
    }

    // Recursively verify children
    if (node.children) {
      if (!Array.isArray(node.children)) {
        errors.push(`Node at ${index} has non-array children`);
      } else {
        for (let i = 0; i < node.children.length; i++) {
          verifyNode(node.children[i]!, depth + 1, `${index}.children[${i}]`);
        }
      }
    }
  }

  for (let i = 0; i < nodes.length; i++) {
    verifyNode(nodes[i]!, 0, `[${i}]`);
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Verifies that references graph has valid nodes and edges structure.
 * @param graph - References graph object
 * @returns Verification result with any errors
 */
export function verifyReferencesGraph(graph: ReferencesGraph): VerificationResult {
  const errors: string[] = [];

  if (!graph || typeof graph !== "object") {
    errors.push("Graph must be an object");
    return { valid: false, errors };
  }

  if (!("nodes" in graph)) {
    errors.push("Graph missing required property: nodes");
  } else if (!Array.isArray(graph.nodes)) {
    errors.push("Graph.nodes must be an array");
  } else {
    for (let i = 0; i < graph.nodes.length; i++) {
      const node = graph.nodes[i];
      if (!node) {
        errors.push(`Node at index ${i} is null or undefined`);
        continue;
      }

      if (!("id" in node)) {
        errors.push(`Graph node at index ${i} missing required property: id`);
      }

      if (!("name" in node)) {
        errors.push(`Graph node at index ${i} missing required property: name`);
      }

      if (!("uri" in node)) {
        errors.push(`Graph node at index ${i} missing required property: uri`);
      }

      if (!("range" in node)) {
        errors.push(`Graph node at index ${i} missing required property: range`);
      }
    }
  }

  if (!("edges" in graph)) {
    errors.push("Graph missing required property: edges");
  } else if (!Array.isArray(graph.edges)) {
    errors.push("Graph.edges must be an array");
  } else {
    for (let i = 0; i < graph.edges.length; i++) {
      const edge = graph.edges[i];
      if (!edge) {
        errors.push(`Edge at index ${i} is null or undefined`);
        continue;
      }

      if (!("from" in edge)) {
        errors.push(`Graph edge at index ${i} missing required property: from`);
      }

      if (!("to" in edge)) {
        errors.push(`Graph edge at index ${i} missing required property: to`);
      }

      if (!("type" in edge)) {
        errors.push(`Graph edge at index ${i} missing required property: type`);
      }
    }
  }

  return { valid: errors.length === 0, errors };
}

/**
 * Helper to check if a verification result is valid.
 * @param result - Verification result
 * @returns True if valid
 */
export function isValid(result: VerificationResult): boolean {
  return result.valid;
}

/**
 * Helper to assert that a verification result is valid.
 * @param result - Verification result
 * @throws Error if invalid
 */
export function assertValid(result: VerificationResult): void {
  if (!result.valid) {
    throw new Error(`Verification failed: ${result.errors.join(", ")}`);
  }
}
