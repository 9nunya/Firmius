#include "memory/RecallIndex.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace firmius::core::memory {

namespace {

std::string toLower(std::string_view sv) {
  std::string out(sv);
  for (auto &c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::chrono::system_clock::time_point now() {
  return std::chrono::system_clock::now();
}

float ageSeconds(const std::chrono::system_clock::time_point &tp) {
  return std::chrono::duration<float>(now() - tp).count();
}

} // namespace

std::vector<std::string> RecallIndex::tokenize(std::string_view text) const {
  std::vector<std::string> tokens;
  std::string current;
  for (char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
      current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty())
    tokens.push_back(std::move(current));
  return tokens;
}

std::vector<std::string>
RecallIndex::extractFilePaths(std::string_view text) const {
  static const std::regex pathRe(
      R"((?:^|\s)(/[\w./\-]+\.\w+|[\w./\-]+/\w+\.\w+))");
  std::vector<std::string> paths;
  std::string s(text);
  for (std::sregex_iterator it(s.begin(), s.end(), pathRe), end; it != end;
       ++it) {
    paths.push_back((*it)[1].str());
  }
  return paths;
}

void RecallIndex::addEvent(IndexedEvent event) {
  EventId id = event.id;
  uint32_t docLen = event.docLength;
  size_t idx = nextIdx_++;

  eventIdx_[id] = idx;
  events_.push_back(std::move(event));
  totalDocLength_ += docLen;

  auto terms = tokenize(events_[idx].content);
  std::unordered_map<std::string, uint32_t> tf;
  for (auto &t : terms)
    tf[t]++;
  for (auto &[term, freq] : tf) {
    inverted_[term].push_back({id, freq});
  }

  auto paths = events_[idx].filePaths;
  if (paths.empty())
    paths = extractFilePaths(events_[idx].content);
  for (auto &p : paths) {
    fileToEvents_[p].insert(id);
    eventToFiles_[id].insert(p);
  }

  for (auto &a : events_[idx].anchors) {
    anchorToEvents_[toLower(a)].insert(id);
  }

  dedupMap_[events_[idx].contentHash] =
      "↪ identical to event #" + std::to_string(id);
}

float RecallIndex::bm25Score(EventId eventId,
                             const std::vector<std::string> &terms) const {
  auto it = eventIdx_.find(eventId);
  if (it == eventIdx_.end())
    return 0.0f;

  size_t idx = it->second;
  float N = static_cast<float>(events_.size());
  float avgDl = events_.empty()
                    ? 1.0f
                    : static_cast<float>(totalDocLength_) / N;
  float dl = static_cast<float>(events_[idx].docLength);
  float score = 0.0f;

  for (auto &term : terms) {
    auto inv = inverted_.find(term);
    if (inv == inverted_.end())
      continue;

    uint32_t tf = 0;
    for (auto &p : inv->second) {
      if (p.eventId == eventId) {
        tf = p.termFreq;
        break;
      }
    }
    if (tf == 0)
      continue;

    float df = static_cast<float>(inv->second.size());
    float idf = std::log((N - df + 0.5f) / (df + 0.5f) + 1.0f);
    float tfNorm = (tf * (k1 + 1.0f)) /
                   (tf + k1 * (1.0f - b + b * (dl / avgDl)));
    score += idf * tfNorm;
  }
  return score;
}

float RecallIndex::recencyScore(const IndexedEvent &ev) const {
  float age = ageSeconds(ev.timestamp);
  return 1.0f / (1.0f + age / 60.0f);
}

float RecallIndex::fileMatchScore(
    const IndexedEvent &ev,
    const std::vector<std::string> &activeFiles) const {
  if (activeFiles.empty())
    return 0.0f;

  auto it = eventToFiles_.find(ev.id);
  if (it == eventToFiles_.end())
    return 0.0f;

  float hits = 0;
  for (auto &f : activeFiles) {
    if (it->second.count(f))
      hits += 1.0f;
  }
  return hits / static_cast<float>(activeFiles.size());
}

std::vector<ScoredEvent> RecallIndex::recall(const Query &query,
                                             size_t k) const {
  std::unordered_set<EventId> candidateIds;

  auto terms = tokenize(query.text);
  for (auto &term : terms) {
    auto it = inverted_.find(term);
    if (it != inverted_.end())
      for (auto &p : it->second)
        candidateIds.insert(p.eventId);
  }

  for (auto &f : query.activeFiles) {
    auto it = fileToEvents_.find(f);
    if (it != fileToEvents_.end())
      for (auto id : it->second)
        candidateIds.insert(id);
  }

  for (auto &a : query.activeAnchors) {
    auto it = anchorToEvents_.find(toLower(a));
    if (it != anchorToEvents_.end())
      for (auto id : it->second)
        candidateIds.insert(id);
  }

  if (candidateIds.empty()) {
    size_t start = events_.size() > k ? events_.size() - k : 0;
    for (size_t i = start; i < events_.size(); ++i)
      candidateIds.insert(events_[i].id);
  }

  std::vector<ScoredEvent> scored;
  for (auto id : candidateIds) {
    auto it = eventIdx_.find(id);
    if (it == eventIdx_.end())
      continue;

    const auto &ev = events_[it->second];
    float score = wR * recencyScore(ev) + wF * fileMatchScore(ev, query.activeFiles) +
                  wB * bm25Score(id, terms);
    scored.push_back({id, score});
  }

  std::sort(scored.begin(), scored.end(),
            [](auto &a, auto &b) { return a.score > b.score; });

  if (scored.size() > k)
    scored.resize(k);
  return scored;
}

std::optional<std::string> RecallIndex::dedup(uint64_t hash) const {
  auto it = dedupMap_.find(hash);
  if (it != dedupMap_.end())
    return it->second;
  return std::nullopt;
}

} // namespace firmius::core::memory
