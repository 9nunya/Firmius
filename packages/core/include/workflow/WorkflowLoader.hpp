#pragma once

#include "workflow/Workflow.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace firmius::core {

/**
 * Loads and manages workflow definitions from ~/.firmius/workflows/
 * Workflows are .md files with optional YAML frontmatter.
 */
class WorkflowLoader {
public:
  static WorkflowLoader &instance();

  WorkflowLoader(const WorkflowLoader &) = delete;
  WorkflowLoader &operator=(const WorkflowLoader &) = delete;

  /**
   * Initialize the workflow loader.
   * Scans the workflows directory and loads all .md files.
   */
  void init();

  /**
   * Get a workflow by its ID (filename without extension).
   * @param id The workflow ID.
   * @return Pointer to the workflow, or nullptr if not found.
   */
  const Workflow *getWorkflow(const std::string &id) const;

  /**
   * Get all loaded workflows.
   * @return Vector of all workflow definitions.
   */
  std::vector<Workflow> getAllWorkflows() const;

  /**
   * Get the list of workflow IDs.
   * @return Vector of workflow IDs.
   */
  std::vector<std::string> getWorkflowIds() const;

  /**
   * Bootstrap default workflows from the builtin directory.
   * Copies workflows from the builtin directory to ~/.firmius/workflows/
   * if the user directory doesn't exist.
   * @param builtinWorkflowsDir Path to the builtin workflows directory.
   */
  static void bootstrapDefaults(const std::string &builtinWorkflowsDir);

private:
  WorkflowLoader() = default;
  ~WorkflowLoader() = default;

  /**
   * Load a single workflow from a file path.
   * @param path Full path to the .md file.
   * @return The loaded workflow, or nullopt if loading failed.
   */
  std::optional<Workflow> loadWorkflow(const std::string &path);
  std::optional<Workflow> loadYamlWorkflow(const std::string &path);
  void loadHookPacks();
  std::vector<std::string> getHookDirs() const;

  /**
   * Get the workflows directory path.
   * Uses FIRMIUS_WORKFLOWS_DIR env var, then ~/.firmius/workflows/,
   * then falls back to "workflows/" for builtin.
   * @return The workflows directory path.
   */
  std::string getWorkflowsDir() const;

  std::map<std::string, Workflow> workflows_;
};

} // namespace firmius::core
