import { describe, expect, it } from "vitest";

import { renderPlaceholderApp } from "../src/app";

describe("renderPlaceholderApp", () => {
  it("renders the scaffold heading", () => {
    expect(renderPlaceholderApp()).toContain("Firmius frontend scaffold");
  });
});
