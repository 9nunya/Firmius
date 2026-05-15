#include "TUIState.hpp"

#include "harness/Harness.hpp"
#include "harness/ThreadLockManager.hpp"
#include "providers/ProviderRegistry.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>

namespace firmius::tui {

namespace {

std::string normalizePath(const std::string &path) {
  if (path.empty()) {
    return path;
  }
  std::error_code ec;
  std::filesystem::path p(path);
  if (!p.is_absolute()) {
    p = std::filesystem::absolute(p, ec);
  }
  if (!ec) {
    p = std::filesystem::weakly_canonical(p, ec);
  }
  return p.string();
}

std::optional<int> lockedPidByOther(const std::string &thread_id) {
  firmius::core::ThreadLockManager lock_manager;
  const int acquire_result = lock_manager.acquire(thread_id);
  if (acquire_result >= 0) {
    lock_manager.release(thread_id);
    return std::nullopt;
  }
  if (acquire_result != -2) {
    return std::nullopt;
  }
  const int pid = lock_manager.getOwnerPid(thread_id);
  return pid > 0 ? std::optional<int>(pid) : std::nullopt;
}

BackgroundTaskPool &ensureBackgroundTaskPool(TuiState &state) {
  if (!state.background_task_pool_) {
    state.background_task_pool_ = std::make_unique<BackgroundTaskPool>(4);
  }
  return *state.background_task_pool_;
}

UiTaskScheduler &ensureUiTaskScheduler(TuiState &state) {
  if (!state.ui_task_scheduler_) {
    state.ui_task_scheduler_ = std::make_unique<UiTaskScheduler>(
        ensureBackgroundTaskPool(state),
        [&state](std::function<void()> action) {
          state.deferUiMutation(std::move(action));
        });
  }
  return *state.ui_task_scheduler_;
}

std::vector<UiProviderRow> buildProviderRows() {
  const auto cfg = firmius::core::Harness::instance().getConfig();
  std::map<std::string, UiProviderRow> merged;

  for (const auto &id :
       firmius::provider::ProviderRegistry::instance().listProviderIds()) {
    auto provider =
        firmius::provider::ProviderRegistry::instance().getProvider(id);
    UiProviderRow row;
    row.id = id;
    row.is_custom = false;
    if (provider) {
      row.kind = provider->getProviderType() ==
                         firmius::provider::ProviderType::OAuth
                     ? "oauth"
                     : "apikey";
      row.enabled = provider->isConfigured();
    } else {
      row.kind = "?";
      row.enabled = false;
    }
    merged[id] = row;
  }

  for (const auto &[id, profile] : cfg.providers) {
    UiProviderRow row;
    row.id = id;
    row.is_custom = true;
    row.kind = profile.kind;
    row.enabled = profile.enabled;
    merged[id] = row;
  }

  std::vector<UiProviderRow> out;
  out.reserve(merged.size());
  for (auto &[_, row] : merged) {
    out.push_back(std::move(row));
  }
  return out;
}

} // namespace

UiThreadListSnapshot TuiState::threadListSnapshot() const {
  std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
  return thread_list_snapshot_;
}

UiProviderSnapshot TuiState::providerSnapshot() const {
  std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
  return provider_snapshot_;
}

UiModelSnapshot TuiState::modelSnapshot() const {
  std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
  return model_snapshot_;
}

void TuiState::refreshThreadListSnapshot(std::string cwd, bool show_all,
                                         std::function<void()> on_applied) {
  cwd = normalizePath(cwd);
  {
    std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
    thread_list_snapshot_.loading = true;
    thread_list_snapshot_.show_all = show_all;
    thread_list_snapshot_.scope_cwd = cwd;
  }

  ensureUiTaskScheduler(*this).schedule(
      "snapshot:threads:" + cwd + ":" + (show_all ? "all" : "project"),
      UiTaskScheduler::Priority::VisiblePrefetch,
      [this, cwd = std::move(cwd), show_all, on_applied = std::move(on_applied)](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t generation) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed)) {
          return {};
        }

        auto &harness = firmius::core::Harness::instance();
        auto threads = harness.listThreads();
        std::vector<UiThreadListEntry> entries;
        entries.reserve(threads.size());
        for (const auto &thread : threads) {
          if (!show_all && !cwd.empty() && normalizePath(thread.cwd) != cwd &&
              !thread.isBenchmarkRun) {
            continue;
          }

          UiThreadListEntry entry;
          entry.metadata = thread;
          entry.agent_count =
              static_cast<int>(harness.listAgents(thread.threadId).size());
          const auto locked_pid = lockedPidByOther(thread.threadId);
          entry.locked_by_other = locked_pid.has_value();
          entry.locked_pid = locked_pid.value_or(-1);
          entries.push_back(std::move(entry));
        }

        std::stable_sort(
            entries.begin(), entries.end(),
            [](const UiThreadListEntry &lhs, const UiThreadListEntry &rhs) {
              return lhs.metadata.lastActiveAt > rhs.metadata.lastActiveAt;
            });

        return [this, entries = std::move(entries), cwd, show_all, generation,
                on_applied = std::move(on_applied)]() mutable {
          {
            std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
            thread_list_snapshot_.entries = std::move(entries);
            thread_list_snapshot_.loading = false;
            thread_list_snapshot_.show_all = show_all;
            thread_list_snapshot_.scope_cwd = cwd;
            thread_list_snapshot_.generation = generation;
          }
          if (on_applied) {
            on_applied();
          }
        };
      });
}

void TuiState::refreshProviderSnapshot(std::function<void()> on_applied) {
  {
    std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
    provider_snapshot_.loading = true;
  }

  ensureUiTaskScheduler(*this).schedule(
      "snapshot:providers", UiTaskScheduler::Priority::VisiblePrefetch,
      [this, on_applied = std::move(on_applied)](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t generation) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed)) {
          return {};
        }
        auto rows = buildProviderRows();
        return [this, rows = std::move(rows), generation,
                on_applied = std::move(on_applied)]() mutable {
          {
            std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
            provider_snapshot_.rows = std::move(rows);
            provider_snapshot_.loading = false;
            provider_snapshot_.generation = generation;
          }
          if (on_applied) {
            on_applied();
          }
        };
      });
}

void TuiState::refreshModelSnapshot(std::function<void()> on_applied) {
  {
    std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
    model_snapshot_.loading = true;
  }

  ensureUiTaskScheduler(*this).schedule(
      "snapshot:models", UiTaskScheduler::Priority::VisiblePrefetch,
      [this, on_applied = std::move(on_applied)](
          const std::shared_ptr<std::atomic<bool>> &cancelled,
          std::uint64_t generation) mutable -> UiTaskScheduler::ApplyCallback {
        if (cancelled->load(std::memory_order_relaxed)) {
          return {};
        }
        auto &harness = firmius::core::Harness::instance();
        harness.listAllModels();
        auto models = harness.cachedModelsSnapshot();
        auto fetching = harness.listProvidersFetchingModels();
        const bool loaded = harness.isModelsLoaded();
        return [this, models = std::move(models), fetching = std::move(fetching),
                loaded, generation, on_applied = std::move(on_applied)]() mutable {
          {
            std::lock_guard<std::mutex> lock(ui_snapshot_mutex_);
            model_snapshot_.models = std::move(models);
            model_snapshot_.fetching_providers = std::move(fetching);
            model_snapshot_.loading = !loaded;
            model_snapshot_.loaded = loaded;
            model_snapshot_.generation = generation;
          }
          if (on_applied) {
            on_applied();
          }
        };
      });
}

} // namespace firmius::tui
