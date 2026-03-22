#include "Context.hpp"
#include "utils/ReferenceAutocomplete.hpp"

#include <algorithm>
#include <gtest/gtest.h>

using namespace firmius::tui;
using firmius::shared::ThreadArtifactMetadata;

TEST(ReferenceAutocompleteTest, FileSuggestionsMatchAndSortWithPrefixBias) {
  const std::vector<std::string> files = {
      "src/file.ts", "src/module/file_ref.ts", "docs/file-notes.md", "README.md"};

  const auto suggestions = BuildFileReferenceSuggestions(files, "file");
  ASSERT_EQ(suggestions.size(), 3u);
  EXPECT_EQ(suggestions[0], "docs/file-notes.md");
  EXPECT_EQ(suggestions[1], "src/file.ts");
  EXPECT_EQ(suggestions[2], "src/module/file_ref.ts");
}

TEST(ReferenceAutocompleteTest, ArtifactSuggestionsUseFriendlyNameWhenAmbiguous) {
  ThreadArtifactMetadata a;
  a.ownerFriendlyName = "planner";
  a.ownerAgentId = "agent-a";
  a.filename = "REPORT.md";

  ThreadArtifactMetadata b;
  b.ownerFriendlyName = "auditor";
  b.ownerAgentId = "agent-b";
  b.filename = "REPORT.md";

  ThreadArtifactMetadata c;
  c.ownerFriendlyName = "worker";
  c.ownerAgentId = "agent-c";
  c.filename = "WORKER_REPORT.md";

  const auto suggestions =
      BuildArtifactReferenceSuggestions({a, b, c}, "report");
  ASSERT_EQ(suggestions.size(), 3u);
  EXPECT_NE(std::find(suggestions.begin(), suggestions.end(), "auditor/REPORT.md"),
            suggestions.end());
  EXPECT_NE(std::find(suggestions.begin(), suggestions.end(), "planner/REPORT.md"),
            suggestions.end());
  EXPECT_NE(std::find(suggestions.begin(), suggestions.end(), "WORKER_REPORT.md"),
            suggestions.end());
}

TEST(ReferenceAutocompleteTest, ArtifactSuggestionsKeepUnambiguousFilenameBare) {
  ThreadArtifactMetadata a;
  a.ownerFriendlyName = "executor";
  a.ownerAgentId = "agent-x";
  a.filename = "EXECUTION_REPORT.md";

  const auto suggestions =
      BuildArtifactReferenceSuggestions({a}, "execution");
  ASSERT_EQ(suggestions.size(), 1u);
  EXPECT_EQ(suggestions[0], "EXECUTION_REPORT.md");
}
