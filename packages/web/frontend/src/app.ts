export function renderPlaceholderApp(): string {
  return [
    "<section data-testid=\"frontend-placeholder\">",
    "  <h1>Firmius frontend scaffold</h1>",
    "  <p>Package-local TypeScript build/test wiring is ready for later chunks.</p>",
    "</section>",
  ].join("\n");
}
