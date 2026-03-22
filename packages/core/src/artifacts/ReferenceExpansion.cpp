#include "artifacts/ReferenceExpansion.hpp"
#include "persistence/ThreadManager.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace firmius::core::artifacts {

namespace {

struct Replacement {
  std::size_t start = 0;
  std::size_t length = 0;
  std::string value;
};

std::string threadStorageRootPath() {
  if (const char *home = std::getenv("HOME")) {
    return std::string(home) + "/.firmius/threads";
  }
  return ".firmius/threads";
}

bool hasReferenceBoundary(const std::string &text, std::size_t atPos) {
  if (atPos == 0) {
    return true;
  }
  const unsigned char prev = static_cast<unsigned char>(text[atPos - 1]);
  if (std::isspace(prev)) {
    return true;
  }
  switch (prev) {
  case '(':
  case '[':
  case '{':
  case '"':
  case '\'':
  case '`':
  case '>':
  case ':':
    return true;
  default:
    return false;
  }
}

bool isTrailingPunctuation(char c) {
  switch (c) {
  case '.':
  case ',':
  case ';':
  case ':':
  case '!':
  case '?':
  case ')':
  case ']':
  case '}':
    return true;
  default:
    return false;
  }
}

std::string xmlEscape(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (char c : input) {
    switch (c) {
    case '&':
      output += "&amp;";
      break;
    case '<':
      output += "&lt;";
      break;
    case '>':
      output += "&gt;";
      break;
    case '"':
      output += "&quot;";
      break;
    case '\'':
      output += "&apos;";
      break;
    default:
      output.push_back(c);
      break;
    }
  }
  return output;
}

std::string readFileContent(const std::filesystem::path &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("File reference not found: " + path.string());
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string readFileContentRange(const std::filesystem::path &path, int startLine,
                                 int endLine) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("File reference not found: " + path.string());
  }

  std::vector<std::string> lines;
  std::string line;
  while (std::getline(file, line)) {
    lines.push_back(line);
  }

  if (startLine <= 0 || endLine <= 0 || endLine < startLine) {
    throw std::runtime_error("Invalid file line range: " +
                             std::to_string(startLine) + "-" +
                             std::to_string(endLine));
  }
  if (static_cast<std::size_t>(endLine) > lines.size()) {
    throw std::runtime_error("File line range out of bounds for " +
                             path.string() + ": " +
                             std::to_string(startLine) + "-" +
                             std::to_string(endLine));
  }

  std::ostringstream selected;
  for (int i = startLine; i <= endLine; ++i) {
    if (i > startLine) {
      selected << '\n';
    }
    selected << lines[static_cast<std::size_t>(i - 1)];
  }
  return selected.str();
}

std::string expandFileReference(const std::string &token, const std::string &cwd) {
  static const std::regex kFilePattern(
      R"(^@([A-Za-z0-9_./\-]+)(?::([0-9]+)-([0-9]+))?$)");
  std::smatch match;
  if (!std::regex_match(token, match, kFilePattern)) {
    throw std::runtime_error("Malformed file reference: " + token);
  }

  const std::string relativePath = match[1].str();
  std::filesystem::path resolvedPath =
      std::filesystem::path(relativePath).is_absolute()
          ? std::filesystem::path(relativePath)
          : (std::filesystem::path(cwd) / relativePath);
  resolvedPath = resolvedPath.lexically_normal();

  if (!std::filesystem::exists(resolvedPath) ||
      !std::filesystem::is_regular_file(resolvedPath)) {
    throw std::runtime_error("File reference not found: " + relativePath);
  }

  if (match[2].matched && match[3].matched) {
    const int startLine = std::stoi(match[2].str());
    const int endLine = std::stoi(match[3].str());
    const std::string content =
        readFileContentRange(resolvedPath, startLine, endLine);
    std::ostringstream xml;
    xml << "<file path=\"" << xmlEscape(relativePath) << "\" lines=\""
        << startLine << "-" << endLine << "\">";
    if (!content.empty()) {
      xml << "\n" << xmlEscape(content) << "\n";
    }
    xml << "</file>";
    return xml.str();
  }

  const std::string content = readFileContent(resolvedPath);
  std::ostringstream xml;
  xml << "<file path=\"" << xmlEscape(relativePath) << "\">";
  if (!content.empty()) {
    xml << "\n" << xmlEscape(content) << "\n";
  }
  xml << "</file>";
  return xml.str();
}

std::string expandArtifactReference(
    const std::string &token, const std::string &threadId,
    core::ThreadManager &threadManager,
    const std::vector<shared::ThreadArtifactMetadata> &artifacts) {
  static constexpr const char *kPrefix = "@artifact:";
  if (token.rfind(kPrefix, 0) != 0) {
    throw std::runtime_error("Malformed artifact reference: " + token);
  }

  std::string selector = token.substr(std::char_traits<char>::length(kPrefix));
  if (selector.empty()) {
    throw std::runtime_error("Malformed artifact reference: " + token);
  }

  std::string ownerFriendlyName;
  std::string filename;
  const std::size_t slashPos = selector.find('/');
  if (slashPos == std::string::npos) {
    filename = selector;
  } else {
    if (slashPos == 0 || slashPos + 1 >= selector.size()) {
      throw std::runtime_error("Malformed artifact reference: " + token);
    }
    ownerFriendlyName = selector.substr(0, slashPos);
    filename = selector.substr(slashPos + 1);
  }

  if (filename.empty()) {
    throw std::runtime_error("Malformed artifact reference: " + token);
  }

  std::string ownerAgentId;
  if (!ownerFriendlyName.empty()) {
    auto resolved =
        threadManager.findAgentIdByFriendlyName(threadId, ownerFriendlyName);
    if (!resolved.has_value()) {
      throw std::runtime_error("Unknown or ambiguous artifact owner friendly "
                               "name: " +
                               ownerFriendlyName);
    }
    ownerAgentId = *resolved;
  } else {
    std::vector<shared::ThreadArtifactMetadata> matches;
    for (const auto &artifact : artifacts) {
      if (artifact.filename == filename) {
        matches.push_back(artifact);
      }
    }
    if (matches.empty()) {
      throw std::runtime_error("Artifact not found: " + filename);
    }
    if (matches.size() > 1) {
      throw std::runtime_error("Artifact reference is ambiguous for '" +
                               filename +
                               "'. Use @artifact:friendly-name/" + filename);
    }
    ownerAgentId = matches.front().ownerAgentId;
    if (ownerFriendlyName.empty()) {
      ownerFriendlyName = matches.front().ownerFriendlyName;
    }
  }

  shared::ThreadArtifactMetadata metadata;
  bool found = false;
  for (const auto &artifact : artifacts) {
    if (artifact.ownerAgentId == ownerAgentId && artifact.filename == filename) {
      metadata = artifact;
      found = true;
      break;
    }
  }
  if (!found) {
    throw std::runtime_error("Artifact not found: " + token);
  }
  const std::string content =
      threadManager.readArtifact(threadId, ownerAgentId, filename);

  const std::string displayOwner = !metadata.ownerFriendlyName.empty()
                                       ? metadata.ownerFriendlyName
                                       : (!ownerFriendlyName.empty()
                                              ? ownerFriendlyName
                                              : ownerAgentId);
  const std::string displayPath = displayOwner + "/" + filename;

  std::ostringstream xml;
  xml << "<artifact path=\"" << xmlEscape(displayPath) << "\">";
  if (!content.empty()) {
    xml << "\n" << xmlEscape(content) << "\n";
  }
  xml << "</artifact>";
  return xml.str();
}

std::string expandSingleReference(
    const std::string &token, const std::string &threadId, const std::string &cwd,
    core::ThreadManager &threadManager,
    const std::vector<shared::ThreadArtifactMetadata> &artifacts) {
  if (token.rfind("@artifact:", 0) == 0) {
    return expandArtifactReference(token, threadId, threadManager, artifacts);
  }
  return expandFileReference(token, cwd);
}

} // namespace

std::string expandInboundReferences(const std::string &threadId,
                                    const std::string &cwd,
                                    const std::string &text) {
  if (text.empty()) {
    return text;
  }

  core::ThreadManager threadManager(threadStorageRootPath());
  const auto artifacts = threadManager.listArtifacts(threadId);

  static const std::regex kRecognizedPattern(
      R"(@artifact:[^\s]+|@[A-Za-z0-9_./\-]+(?::[0-9]+-[0-9]+)?)");
  std::vector<Replacement> replacements;
  for (std::sregex_iterator it(text.begin(), text.end(), kRecognizedPattern),
       end;
       it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t length = static_cast<std::size_t>(it->length());
    if (!hasReferenceBoundary(text, start)) {
      continue;
    }
    const std::string token = it->str();
    replacements.push_back(
        {start, length,
         expandSingleReference(token, threadId, cwd, threadManager, artifacts)});
  }

  static const std::regex kTokenPattern(R"(@[^\s]+)");
  for (std::sregex_iterator it(text.begin(), text.end(), kTokenPattern), end;
       it != end; ++it) {
    const std::size_t start = static_cast<std::size_t>(it->position());
    const std::size_t length = static_cast<std::size_t>(it->length());
    if (!hasReferenceBoundary(text, start)) {
      continue;
    }

    auto replacementIt = std::find_if(
        replacements.begin(), replacements.end(),
        [&](const Replacement &replacement) { return replacement.start == start; });
    if (replacementIt == replacements.end()) {
      throw std::runtime_error("Malformed reference syntax: " + it->str());
    }

    if (replacementIt->length < length) {
      const std::string suffix = it->str().substr(replacementIt->length);
      for (char c : suffix) {
        if (!isTrailingPunctuation(c)) {
          throw std::runtime_error("Malformed reference syntax: " + it->str());
        }
      }
    }
  }

  if (replacements.empty()) {
    return text;
  }

  std::sort(replacements.begin(), replacements.end(),
            [](const Replacement &lhs, const Replacement &rhs) {
              return lhs.start < rhs.start;
            });

  std::string expanded;
  expanded.reserve(text.size() * 2);
  std::size_t cursor = 0;
  for (const auto &replacement : replacements) {
    if (replacement.start < cursor) {
      continue;
    }
    expanded.append(text, cursor, replacement.start - cursor);
    expanded += replacement.value;
    cursor = replacement.start + replacement.length;
  }
  expanded.append(text, cursor, std::string::npos);
  return expanded;
}

} // namespace firmius::core::artifacts
