#pragma once

#include <map>
#include <string>
#include <vector>

namespace firmius::tui {

inline const std::map<std::string, std::vector<std::string>>& livePhraseBanks() {
  static const std::map<std::string, std::vector<std::string>> kBanks = {
      {"thinking",
       {
           "Thinking through the blast radius...",
           "Reading just enough to avoid doing something stupid.",
           "Naming the uncertainty before we touch code.",
           "Trying not to invent bugs with confidence..",
           "Re-reading the file because skimming is how regressions happen..",
           "Asking myself if I actually understand the failure mode..",
           "Squinting at this SSE chunk like it owes me money..",
           "Deciding which dialect of `thinking` this provider is feeling today..",
       }},
      {"editing",
       {
           "Landing a surgical fix.. hopefully..",
           "Applying hunks with extreme focus..",
           "Trading jank for causality..",
           "Turning requirements into actual code..",
           "Aligning the diff so the gutter doesn't lie about line 312..",
           "Highlighting the bash so you can see the typo before I run it..",
           "Forwarding tool-call deltas without losing a single curly brace..",
           "Patching the parser to admit Bedrock and OpenAI exist..",
       }},
      {"waiting",
       {
           "Waiting for the worker thread to actually do the thing..",
           "Holding position while the provider catches up..",
           "Keeping the event loop warm..",
           "Letting tree-sitter finish parsing before we paint..",
           "Waiting on /getUsageLimits to admit which plan you're on..",
       }},
      {"working",
       {
           "Mapping the dependencies of this mystery subsystem..",
           "Reading the repo's mind..",
           "Scanning for the code that was written in a hurry..",
           "Wrapping that one-liner that grew into a 400-character monster..",
           "Picking an account whose tier is allowed to talk to this model..",
       }},
      {"idle", {"Standing by."}},
  };
  return kBanks;
}

inline std::vector<std::string> livePhrasesForMode(const std::string& mode) {
  const auto& banks = livePhraseBanks();
  auto it = banks.find(mode);
  return it == banks.end() ? banks.at("idle") : it->second;
}

} // namespace firmius::tui
