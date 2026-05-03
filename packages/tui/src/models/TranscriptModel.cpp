#include "models/TranscriptModel.hpp"

namespace firmius::tui {

TranscriptModel& TranscriptModel::instance() {
  static TranscriptModel inst;
  return inst;
}

} // namespace firmius::tui
