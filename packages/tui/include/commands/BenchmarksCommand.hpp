#pragma once

#include "ICommand.hpp"

namespace firmius::tui {

class BenchmarksCommand : public ICommand {
public:
  std::string name() const override { return "benchmarks"; }
  std::string description() const override {
    return "Run a benchmark task from welcome screen (usage: /benchmarks <id> [task_id])";
  }
  std::vector<CommandArg> args() const override {
    return {CommandArg{"benchmark", ArgType::String, "Benchmark id", true},
            CommandArg{"task_id", ArgType::String, "Optional task id", true}};
  }
  void execute(CommandCtx &ctx, const std::vector<ParsedArg> &args) override;
};

} // namespace firmius::tui

