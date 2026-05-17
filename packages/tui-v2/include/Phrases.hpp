#pragma once

#include <map>
#include <string>
#include <vector>

namespace firmius::tui2 {

inline const std::map<std::string, std::vector<std::string>>& livePhraseBanks() {
  static const std::map<std::string, std::vector<std::string>> kBanks = {
      {"thinking",
       {
           "Thinking through the blast radius...",
           "Reading just enough to avoid doing something stupid.",
           "Naming the uncertainty before we touch code.",
           "Trying not to invent bugs with confidence..",
       }},
      {"editing",
       {
           "Landing a surgical fix.. hopefully..",
           "Applying hunks with extreme focus..",
           "Trading jank for causality..",
           "Turning requirements into actual code..",
       }},
      {"waiting",
       {
           "Waiting for the worker thread to actually do the thing..",
           "Holding position while the provider catches up..",
           "Keeping the event loop warm..",
       }},
      {"working",
       {
           "Mapping the dependencies of this mystery subsystem..",
           "Reading the repo's mind..",
           "Scanning for the code that was written in a hurry..",
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

} // namespace firmius::tui2
