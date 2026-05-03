#ifndef FIRMIUS_SHARED_STATUS_ZONE_HPP
#define FIRMIUS_SHARED_STATUS_ZONE_HPP

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace firmius::shared {

// ─── Surface card primitive ─────────────────────────────────────────────────
//
// SurfaceCard is the portable status-data primitive emitted by core
// subsystems (Pact, Workflow, Memory, Mode, Branch). Surfaces (TUI, future
// Discord/Telegram/etc.) translate it into their native UX. Subsystems
// never know which surface renders them; surfaces never know which
// subsystem emitted what.

enum class CardLifetime {
  /// Renders until explicitly removed.
  Sticky,
  /// Renders until the subsystem signals the bound event closed.
  EventBound,
  /// Auto-removes after `ephemeralFor` elapses.
  Ephemeral,
};

enum class CardAccent {
  Neutral,
  Info,
  Success,
  Warning,
  Failure,
  Active,
};

struct CardField {
  std::string label;
  std::string value;
};

struct CardAction {
  std::string id;     // machine identifier (e.g. "pact.retry")
  std::string label;  // human label ("Retry")
  std::optional<std::string> shortcut;  // e.g. "ctrl+r"
};

struct SurfaceCard {
  std::string id;                     ///< stable identifier for in-place updates
  std::string subsystemId;            ///< "pact" | "workflow" | "memory" | ...
  std::string title;                  ///< first line / header
  std::optional<std::string> subtitle;
  CardAccent accent = CardAccent::Neutral;
  std::vector<CardField> fields;      ///< structured key/value rows
  std::vector<CardAction> actions;
  CardLifetime lifetime = CardLifetime::EventBound;
  std::optional<std::chrono::milliseconds> ephemeralFor;
  std::int64_t createdAtMs = 0;       ///< wall-clock millis (set by emitter)
};

// ─── Band priority ──────────────────────────────────────────────────────────

enum class BandPriority : int {
  Pact = 10,        ///< highest visibility — verification-in-progress
  Workflow = 20,
  Branch = 30,
  Mode = 40,
  Memory = 50,      ///< sliver, lowest-priority always-on
  Custom = 1000,
};

// ─── IStatusBand ────────────────────────────────────────────────────────────
//
// A band is a renderable strip in the status zone owned by a subsystem.
// Each implementation knows how to render itself per skin id; the controller
// is skin-agnostic. Cards are passed via update() so a subsystem can publish
// state changes without re-implementing band internals.

class IStatusBand {
public:
  virtual ~IStatusBand() = default;

  /// Stable identifier (subsystem-scoped, unique within a zone).
  virtual std::string id() const = 0;

  /// Sort key. Lower priority = renders higher (closer to transcript).
  virtual BandPriority priority() const = 0;

  /// Lines this band wants to occupy at the given width. Renderer-honoured;
  /// the controller may collapse the band if total height exceeds budget.
  virtual int desiredHeight(int width) const = 0;

  /// Replace the band's current card payload. Renderers re-render on next
  /// frame. Passing std::nullopt clears the band but keeps it registered.
  virtual void update(std::optional<SurfaceCard> card) = 0;

  /// Read the current card. Used by alternate surfaces (Discord embed,
  /// Telegram inline keyboard) which read state synchronously.
  virtual std::optional<SurfaceCard> currentCard() const = 0;
};

// ─── Default in-memory band ─────────────────────────────────────────────────
//
// Subsystems that don't need custom rendering can drop a SimpleStatusBand
// into the controller; surfaces translate the card directly. Subsystems
// that DO need custom rendering subclass IStatusBand.

class SimpleStatusBand : public IStatusBand {
public:
  SimpleStatusBand(std::string idStr, BandPriority p)
      : id_(std::move(idStr)), priority_(p) {}

  std::string id() const override { return id_; }
  BandPriority priority() const override { return priority_; }

  int desiredHeight(int /*width*/) const override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!card_.has_value()) {
      return 0;
    }
    int rows = 1; // title
    if (card_->subtitle.has_value() && !card_->subtitle->empty()) {
      rows += 1;
    }
    rows += static_cast<int>(card_->fields.size());
    return rows;
  }

  void update(std::optional<SurfaceCard> card) override {
    std::lock_guard<std::mutex> lock(mu_);
    card_ = std::move(card);
  }

  std::optional<SurfaceCard> currentCard() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return card_;
  }

private:
  std::string id_;
  BandPriority priority_;
  mutable std::mutex mu_;
  std::optional<SurfaceCard> card_;
};

// ─── StatusZoneController ───────────────────────────────────────────────────
//
// Per-thread/agent owner of registered bands. Surfaces (TuiStatusZone,
// DiscordSurface, etc.) read the controller's snapshot to render.
// Subsystems write via registerBand / updateBand.

class StatusZoneController {
public:
  /// Register a band. Idempotent on id — re-registering replaces.
  void registerBand(std::shared_ptr<IStatusBand> band) {
    if (!band) return;
    std::lock_guard<std::mutex> lock(mu_);
    bands_[band->id()] = std::move(band);
    ++version_;
  }

  /// Remove a band by id. No-op if not present.
  void unregisterBand(const std::string &id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (bands_.erase(id) > 0) {
      ++version_;
    }
  }

  /// Convenience: update a SimpleStatusBand's card. Auto-creates the band
  /// at the given priority if missing. Most subsystems will use this and
  /// never touch IStatusBand directly.
  void publishCard(const std::string &bandId, BandPriority priority,
                   std::optional<SurfaceCard> card) {
    std::shared_ptr<IStatusBand> band;
    {
      std::lock_guard<std::mutex> lock(mu_);
      auto it = bands_.find(bandId);
      if (it == bands_.end()) {
        auto created = std::make_shared<SimpleStatusBand>(bandId, priority);
        bands_[bandId] = created;
        band = created;
      } else {
        band = it->second;
      }
      ++version_;
    }
    band->update(std::move(card));
  }

  /// Snapshot the active bands sorted by priority. Returned shared_ptrs are
  /// safe to read while the controller continues mutating elsewhere because
  /// IStatusBand implementations are individually mutex-protected.
  std::vector<std::shared_ptr<IStatusBand>> snapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::shared_ptr<IStatusBand>> out;
    out.reserve(bands_.size());
    for (const auto &[_, b] : bands_) {
      out.push_back(b);
    }
    std::sort(out.begin(), out.end(),
              [](const std::shared_ptr<IStatusBand> &a,
                 const std::shared_ptr<IStatusBand> &b) {
                return static_cast<int>(a->priority()) <
                       static_cast<int>(b->priority());
              });
    return out;
  }

  /// Bumped on every mutation. Surfaces poll this to decide whether to
  /// re-render. Cheap.
  std::uint64_t version() const {
    std::lock_guard<std::mutex> lock(mu_);
    return version_;
  }

private:
  mutable std::mutex mu_;
  std::map<std::string, std::shared_ptr<IStatusBand>> bands_;
  std::uint64_t version_ = 0;
};

} // namespace firmius::shared

#endif
