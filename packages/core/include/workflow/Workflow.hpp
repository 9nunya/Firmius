#pragma once

#include <string>
#include <vector>

namespace firmius::core {

/**
 * Represents a workflow definition loaded from a .md file.
 * Workflows support YAML frontmatter and argument placeholders ($1, $2, etc.)
 */
struct Workflow {
  std::string id;           // Filename without extension (e.g., "parallel_exploration")
  std::string name;         // Human-readable name from frontmatter
  std::string description;  // Description from frontmatter for autocomplete help
  std::string body;         // The workflow body with $1, $2, etc. placeholders
  size_t argCount = 0;      // Number of arguments detected in body

  /**
   * Replace $1, $2, etc. placeholders with actual argument values.
   * @param args Vector of argument values to substitute.
   * @return The workflow body with all placeholders replaced.
   */
  std::string build(const std::vector<std::string> &args) const;
};

} // namespace firmius::core
