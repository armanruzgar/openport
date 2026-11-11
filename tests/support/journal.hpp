#pragma once

#include <memory>
#include <string>

#include "openport/trading/journal.hpp"

namespace openport::test {

/// Journal options for tests of what is written, not of whether it survives a crash:
/// each record is written in full as always, but without the full device sync that
/// every record takes by default (F_FULLFSYNC on macOS, milliseconds apiece), which
/// was most of a long journal test's time. Durability is tested with its own hooks
/// in trading/journal_test.cpp.
inline trading::FileJournal::Options unsynced_journal() {
  trading::FileJournal::Options options;
  options.hooks.sync = [](int) { return true; };
  return options;
}

inline std::shared_ptr<trading::FileJournal> create_journal(const std::string& path) {
  return trading::FileJournal::create(path, unsynced_journal());
}

inline std::shared_ptr<trading::FileJournal> resume_journal(const std::string& path) {
  return trading::FileJournal::resume(path, unsynced_journal());
}

}  // namespace openport::test
