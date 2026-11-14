#pragma once
#include "crypto.hpp"
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
namespace sniper {
// Best-effort lower scheduling priority for background I/O threads. A
// refusal keeps normal priority; nothing depends on it.
inline void background_priority() {
  errno = 0;
  if (::nice(5) == -1 && errno != 0)
    errno = 0;
}
class FileLock {
  int fd_ = -1;

public:
  explicit FileLock(const std::filesystem::path &);
  ~FileLock();
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
};
void atomic_file(const std::filesystem::path &, std::string_view);
class DurableLog {
  std::filesystem::path path_;
  int fd_ = -1;
  std::uint64_t seq_ = 0;
  bool failed_ = false;

public:
  // Only the benchmark binary sets this (to time the hot path without the
  // disk); nothing in the bot can, so live intents are always synced.
  static inline bool benchmark_without_sync = false;
  explicit DurableLog(const std::filesystem::path &);
  ~DurableLog();
  DurableLog(const DurableLog &) = delete;
  DurableLog &operator=(const DurableLog &) = delete;
  void replay(const std::function<void(std::uint64_t, const Json &)> &);
  std::uint64_t append(const Json &);
  std::uint64_t sequence() const { return seq_; }
};
class Journal {
  struct Row {
    std::uint32_t size = 0;
    std::array<char, 8192> data;
  };
  static constexpr std::size_t Capacity = 2048;
  std::unique_ptr<Row[]> rows_;
  std::atomic<std::size_t> head_{0}, tail_{0};
  std::atomic<bool> stop_{false}, failed_{false};
  std::thread thread_;
  int fd_ = -1;

public:
  explicit Journal(const std::filesystem::path &);
  ~Journal();
  bool write(Json row);
  bool failed() const { return failed_.load(); }
  void close();
};
struct Position {
  std::string condition, token;
  Units shares = 0, cost = 0, fees = 0;
  bool settled = false;
  // What one share paid at settlement (micro-dollars); -1 while unknown.
  // Kept so the settled result of a round, and what a winning position is
  // still worth until it is redeemed, can be read back from the ledger.
  Units payout = -1;
};
struct Intent {
  std::string id, condition, token, side, slug, up, down, status = "intent";
  std::int64_t start = 0, end = 0;
  Units stake = 0, reserve = 0, target = 0, limit = 0, matched_shares = 0,
        booked_shares = 0, matched_cost = 0, booked_cost = 0, booked_fee = 0,
        fee_rate = 0;
  int fee_exponent = 1;
  std::vector<std::string> trades;
  bool fill_slot = false, min_entry = false;
  // SNIPER_REST_LIVE: a post-only order resting on the book (a maker: no fee),
  // placed after a FAK that matched nothing. It stays "pending" until the
  // venue's record of it, or its cancellation, gives its final size.
  bool rest = false;
  bool canary_slot_owned = false;
  bool canary_spend_released = false;
  double canary_session = 0;
  double sent_wall = 0;
  bool unresolved() const {
    return status == "intent" || status == "sent" || status == "unknown" ||
           status == "pending";
  }
};
struct CanaryState {
  Units orders = 0, fills = 0, spend = 0, settled_loss = 0;
  std::set<std::string> rounds;
  std::map<std::string, Units> increments;
  double pause_until = 0, started = 0;
  std::string pause_reason;
  Json json() const;
  void load(const Json &);
  bool operator==(const CanaryState &) const = default;
};
class Ledger {
  std::filesystem::path path_;
  DurableLog wal_;
  std::unique_ptr<FileLock> lock_;
  std::map<std::string, std::set<std::string>> pending_;
  std::map<std::string, int> round_fills_;
  std::set<std::string> counted_orders_;
  void apply(const Json &event);
  void commit(Json event);
  void reindex(const std::string &);
  void restore_fill_slot(std::string_view);

public:
  std::map<std::string, Position> positions;
  std::map<std::string, Intent> orders;
  std::set<std::string> trade_ids, settled_conditions, minimum_attempts;
  Units cash = 0, realized = 0, fees = 0, reference_cash = 0, high_water = 0;
  // Settled profit and loss of the resting orders' own fills, all time; the
  // SNIPER_REST_LIVE loss stop reads its change since the test began.
  Units rest_realized = 0;
  double risk_epoch = 0;
  CanaryState canary;
  std::uint64_t entries = 0;
  Ledger(const std::filesystem::path &, Units start_cash, bool live);
  void snapshot();
  void register_intent(const Intent &, const SignedOrder *,
                       const CanaryState &);
  void status(std::string_view id, std::string_view status, Units matched = 0,
              Units cost = 0);
  bool fill(std::string_view trade, std::string_view id, Units shares,
            Units cost, Units fee);
  bool settle(std::string_view condition,
              const std::map<std::string, Units> &payouts);
  void risk_rebase(Units equity);
  // Writes the canary state; a state equal to the current one writes nothing.
  void set_canary(const CanaryState &);
  void release_fill_slot(std::string_view id);
  void release_missed_spend(std::string_view id);
  Units holding(std::string_view token) const;
  Units risk_shares(std::string_view token) const;
  Units reserved() const;
  Units open_cost() const;
  bool awaiting(std::string_view token) const;
  int round_fills(std::string_view condition) const;
  Json json() const;
};
Json intent_json(const Intent &);
Intent intent_from_json(const Json &);
} // namespace sniper
