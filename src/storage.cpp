#include "sniper/storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace sniper {
static Units checked_money(__int128 value) {
  if (value > std::numeric_limits<Units>::max() || value < std::numeric_limits<Units>::min())
    throw std::overflow_error("ledger monetary total overflow");
  return static_cast<Units>(value);
}
static void all_write(int fd, std::string_view text) {
  while (!text.empty()) {
    auto n = ::write(fd, text.data(), text.size());
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("durable write failed");
    text.remove_prefix(n);
  }
}
FileLock::FileLock(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB)) {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("another process owns this ledger or wallet lock");
  }
}
FileLock::~FileLock() {
  if (fd_ >= 0) {
    flock(fd_, LOCK_UN);
    ::close(fd_);
  }
}
void atomic_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  auto tmp = path.string() + ".tmp";
  int fd = ::open(tmp.c_str(),
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    throw std::runtime_error("snapshot open failed");
  try {
    all_write(fd, content);
    if (::fsync(fd))
      throw std::runtime_error("snapshot sync failed");
    ::close(fd);
    fd = -1;
    if (::rename(tmp.c_str(), path.c_str()))
      throw std::runtime_error("snapshot rename failed");
    int dir =
        ::open(path.parent_path().c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (dir < 0 || ::fsync(dir)) {
      if (dir >= 0)
        ::close(dir);
      throw std::runtime_error("snapshot directory sync failed");
    }
    ::close(dir);
  } catch (...) {
    if (fd >= 0)
      ::close(fd);
    throw;
  }
}
DurableLog::DurableLog(const std::filesystem::path &p) : path_(p) {
  std::filesystem::create_directories(p.parent_path());
  fd_ = ::open(p.c_str(),
               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0)
    throw std::runtime_error("journal open failed");
}
DurableLog::~DurableLog() {
  if (fd_ >= 0)
    ::close(fd_);
}
void DurableLog::replay(
    const std::function<void(std::uint64_t, const Json &)> &apply) {
  std::ifstream in(path_, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read durable journal");
  std::string line;
  std::uint64_t next = 1;
  while (std::getline(in, line)) {
    if (in.eof())
      throw std::runtime_error("damaged durable journal: incomplete last line");
    auto row = Json::parse(line);
    if (row.at("seq").get<std::uint64_t>() != next ||
        row.at("sha256").get<std::string>() != sha256(row.at("event").dump()))
      throw std::runtime_error("damaged durable journal: sequence or checksum");
    apply(next++, row.at("event"));
  }
  if (in.bad())
    throw std::runtime_error("error reading durable journal");
  seq_ = next - 1;
}
std::uint64_t DurableLog::append(const Json &event) {
  // A failed write or sync can leave this row, or part of it, on disk. A later
  // row would then repeat its sequence number and the log would be refused at
  // the next start, so after a failure the log takes no more rows.
  if (failed_)
    throw std::runtime_error("durable log refused: an earlier write failed");
  auto text = Json{{"seq", seq_ + 1},
                   {"event", event},
                   {"sha256", sha256(event.dump())}}
                  .dump() +
              "\n";
  try {
    all_write(fd_, text);
    if (!benchmark_without_sync && fdatasync(fd_))
      throw std::runtime_error("durable intent sync failed");
  } catch (...) {
    failed_ = true;
    throw;
  }
  return ++seq_;
}
Journal::Journal(const std::filesystem::path &p)
    : rows_(std::make_unique<Row[]>(Capacity)) {
  std::filesystem::create_directories(p.parent_path());
  fd_ = ::open(p.c_str(),
               O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd_ < 0)
    throw std::runtime_error("audit journal open failed");
  thread_ = std::thread([this] {
    background_priority();
    while (!stop_ || tail_.load() != head_.load()) {
      auto t = tail_.load(std::memory_order_relaxed),
           h = head_.load(std::memory_order_acquire);
      if (t == h) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      auto &row = rows_[t % Capacity];
      try {
        all_write(fd_, {row.data.data(), row.size});
      } catch (...) {
        failed_ = true;
      }
      tail_.store(t + 1, std::memory_order_release);
    }
    if (fdatasync(fd_))
      failed_ = true;
  });
}
bool Journal::write(Json row) {
  if (failed_ || stop_)
    return false;
  if (!row.contains("wall"))
    row["wall"] = wall_s();
  // Venue error text can carry arbitrary bytes. Replace invalid UTF-8
  // rather than letting dump() throw out of an event handler.
  auto text = row.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
  if (text.size() > 8192) {
    failed_ = true;
    return false;
  }
  auto h = head_.load(std::memory_order_relaxed),
       t = tail_.load(std::memory_order_acquire);
  if (h - t >= Capacity) {
    failed_ = true;
    return false;
  }
  auto &dest = rows_[h % Capacity];
  dest.size = text.size();
  std::memcpy(dest.data.data(), text.data(), text.size());
  head_.store(h + 1, std::memory_order_release);
  return true;
}
void Journal::close() {
  if (stop_.exchange(true))
    return;
  if (thread_.joinable())
    thread_.join();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}
Journal::~Journal() { close(); }
Json CanaryState::json() const {
  return {{"orders", orders},
          {"fills", fills},
          {"spend", spend},
          {"settled_loss", settled_loss},
          {"rounds", rounds},
          {"increments", increments},
          {"pause_until", pause_until},
          {"started", started},
          {"pause_reason", pause_reason}};
}
void CanaryState::load(const Json &j) {
  orders = j.at("orders");
  fills = j.at("fills");
  spend = j.at("spend");
  settled_loss = j.at("settled_loss");
  rounds = j.at("rounds").get<std::set<std::string>>();
  increments = j.at("increments").get<std::map<std::string, Units>>();
  pause_until = j.at("pause_until");
  started = j.value("started", 0.0);
  pause_reason = j.at("pause_reason");
}
Json intent_json(const Intent &i) {
  return {{"id", i.id},
          {"condition", i.condition},
          {"token", i.token},
          {"side", i.side},
          {"slug", i.slug},
          {"up", i.up},
          {"down", i.down},
          {"status", i.status},
          {"start", i.start},
          {"end", i.end},
          {"stake", i.stake},
          {"reserve", i.reserve},
          {"target", i.target},
          {"limit", i.limit},
          {"matched_shares", i.matched_shares},
          {"booked_shares", i.booked_shares},
          {"matched_cost", i.matched_cost},
          {"booked_cost", i.booked_cost},
          {"booked_fee", i.booked_fee},
          {"fee_rate", i.fee_rate},
          {"fee_exponent", i.fee_exponent},
          {"trades", i.trades},
          {"fill_slot", i.fill_slot},
          {"canary_slot_owned", i.canary_slot_owned},
          {"canary_spend_released", i.canary_spend_released},
          {"canary_session", i.canary_session},
          {"min_entry", i.min_entry},
          {"rest", i.rest},
          {"sent_wall", i.sent_wall}};
}
Intent intent_from_json(const Json &j) {
  Intent i;
  i.id = j.at("id");
  i.condition = j.at("condition");
  i.token = j.at("token");
  i.side = j.at("side");
  i.slug = j.at("slug");
  i.up = j.value("up", "");
  i.down = j.value("down", "");
  i.status = j.at("status");
  i.start = j.at("start");
  i.end = j.at("end");
  i.stake = j.at("stake");
  i.reserve = j.at("reserve");
  i.target = j.at("target");
  i.limit = j.at("limit");
  i.matched_shares = j.at("matched_shares");
  i.booked_shares = j.at("booked_shares");
  i.booked_cost = j.value("booked_cost", Units(0));
  i.booked_fee = j.value("booked_fee", Units(0));
  i.fee_rate = j.value("fee_rate", Units(0));
  i.fee_exponent = j.value("fee_exponent", 1);
  i.matched_cost = j.at("matched_cost");
  i.trades = j.at("trades").get<std::vector<std::string>>();
  i.fill_slot = j.at("fill_slot");
  i.canary_slot_owned = j.value("canary_slot_owned", false);
  i.canary_spend_released = j.value("canary_spend_released", false);
  i.canary_session = j.value("canary_session", 0.0);
  i.min_entry = j.at("min_entry");
  i.rest = j.value("rest", false);
  i.sent_wall = j.at("sent_wall");
  return i;
}
Ledger::Ledger(const std::filesystem::path &p, Units start, bool live)
    : path_(p), wal_(p.string() + ".journal"), cash(start),
      reference_cash(live ? 0 : start), high_water(live ? 0 : start) {
  lock_ = std::make_unique<FileLock>(p.string() + ".lock");
  std::uint64_t snapshot_seq = 0;
  const bool has_snapshot = std::filesystem::exists(p);
  if (has_snapshot) {
    std::ifstream f(p);
    Json j;
    f >> j;
    if (j.at("format") != "sniper-native-ledger-1" &&
        j.at("format") != "sniper-native-ledger-2")
      throw std::runtime_error(
          "ledger format differs: use separate native ledger files");
    if (j.at("format") == "sniper-native-ledger-2") {
      auto expected = j.at("sha256").get<std::string>();
      auto body = j;
      body.erase("sha256");
      if (expected != sha256(body.dump()))
        throw std::runtime_error("damaged ledger snapshot: checksum mismatch");
    }
    snapshot_seq = j.at("sequence");
    cash = j.at("cash");
    reference_cash = j.at("reference_cash");
    high_water = j.at("high_water");
    realized = j.at("realized");
    rest_realized = j.value("rest_realized", Units(0));
    fees = j.at("fees");
    risk_epoch = j.at("risk_epoch");
    dd_pause_until = j.at("dd_pause_until");
    for (auto &v : j.at("positions")) {
      Position x;
      x.condition = v.at("condition");
      x.token = v.at("token");
      x.shares = v.at("shares");
      x.cost = v.at("cost");
      x.fees = v.at("fees");
      x.settled = v.at("settled");
      positions[x.token] = x;
    }
    for (auto &v : j.at("orders")) {
      auto i = intent_from_json(v);
      orders[i.id] = i;
    }
    trade_ids = j.at("trade_ids").get<std::set<std::string>>();
    settled_conditions =
        j.at("settled_conditions").get<std::set<std::string>>();
    minimum_attempts = j.at("minimum_attempts").get<std::set<std::string>>();
    canary.load(j.at("canary"));
  }
  for (auto &[id, i] : orders)
    reindex(id);
  // The earlier refund implementation persisted a zero-match status followed
  // immediately by a reduced canary state, without identifying its order in
  // that second event. Recover that attribution from the retained WAL, even
  // when a snapshot has already absorbed both events.
  std::optional<Units> historical_spend;
  double historical_session = 0;
  std::optional<std::string> legacy_miss;
  std::map<std::string, bool> historical_released;
  std::set<double> unsafe_refund_sessions;
  wal_.replay([&](std::uint64_t seq, const Json &e) {
    const auto type = e.at("type").get<std::string>();
    // The retained authorization event identifies the original session even
    // for snapshots written before slot ownership fields were introduced.
    if (type == "intent") {
      auto id = e.at("intent").at("id").get<std::string>();
      auto it = orders.find(id);
      if (it != orders.end() && e.at("intent").value("fill_slot", false)) {
        it->second.canary_slot_owned = true;
        it->second.canary_session = e.at("canary").value("started", 0.0);
      }
    }
    if (type == "canary" && legacy_miss && historical_spend) {
      auto it = orders.find(*legacy_miss);
      const auto &state = e.at("state");
      if (it != orders.end() && it->second.canary_slot_owned &&
          it->second.stake >= 0 && state.value("started", 0.0) == historical_session) {
        const auto expected = std::max<__int128>(0, static_cast<__int128>(*historical_spend) - it->second.stake);
        if (state.at("spend").get<Units>() == expected && expected < *historical_spend) {
          if (historical_released[it->first] || it->second.canary_session != historical_session)
            unsafe_refund_sessions.insert(historical_session);
          historical_released[it->first] = true;
          it->second.canary_spend_released = true;
        }
      }
    }
    legacy_miss.reset();
    if (type == "intent" || type == "canary") {
      const auto &state = e.at(type == "intent" ? "canary" : "state");
      historical_spend = state.at("spend").get<Units>();
      historical_session = state.value("started", 0.0);
      if (type == "intent")
        historical_released[e.at("intent").at("id").get<std::string>()] =
            e.at("intent").value("canary_spend_released", false);
    } else if (type == "release_spend" || type == "restore_spend") {
      auto id = e.at("id").get<std::string>();
      const bool released = type == "release_spend";
      const auto &intent = orders.at(id);
      if (historical_spend && historical_released[id] != released &&
          intent.canary_session == historical_session) {
        historical_spend = released
            ? checked_money(std::max<__int128>(0, static_cast<__int128>(*historical_spend) - intent.stake))
            : checked_money(static_cast<__int128>(*historical_spend) + intent.stake);
      }
      historical_released[id] = released;
      if (seq <= snapshot_seq)
        orders.at(id).canary_spend_released = released;
    } else if (type == "status" && e.at("status") == "no_match" &&
               e.at("shares").get<Units>() == 0 && e.at("cost").get<Units>() == 0) {
      legacy_miss = e.at("id").get<std::string>();
    }
    if (seq > snapshot_seq)
      apply(e);
  });
  if (unsafe_refund_sessions.count(canary.started))
    throw std::runtime_error("legacy canary refund history disagrees with the current session; reconcile its budget before restart");
  if (!has_snapshot && wal_.sequence() != 0)
    throw std::runtime_error("ledger snapshot missing for existing history; initial cash is ambiguous");
  if (snapshot_seq > wal_.sequence())
    throw std::runtime_error("snapshot is ahead of durable journal");
  entries = wal_.sequence();
  if (!has_snapshot)
    snapshot(); // Pin the initial bankroll before any authorization can commit.
  for (const auto &[id, i] : orders) {
    (void)i;
    restore_fill_slot(id);
  }
}
void Ledger::commit(Json e) {
  wal_.append(e);
  apply(e);
  ++entries;
}
void Ledger::apply(const Json &e) {
  auto type = e.at("type").get<std::string>();
  if (type == "intent") {
    auto i = intent_from_json(e.at("intent"));
    if (i.fill_slot) {
      i.canary_slot_owned = true;
      i.canary_session = e.at("canary").value("started", 0.0);
    }
    orders[i.id] = i;
    canary.load(e.at("canary"));
    if (i.min_entry)
      minimum_attempts.insert(i.condition + ":" + i.token);
    reindex(i.id);
  } else if (type == "status") {
    auto &i = orders.at(e.at("id").get<std::string>());
    const bool was_confirmed = i.status == "confirmed";
    i.status = e.at("status");
    i.matched_shares = std::max(i.matched_shares, e.at("shares").get<Units>());
    i.matched_cost = std::max(i.matched_cost, e.at("cost").get<Units>());
    // A late acknowledgement cannot undo confirmed fills. Reindex still
    // reopens the order if this update reveals additional unbooked shares.
    if (was_confirmed && i.unresolved())
      i.status = "matched";
    reindex(i.id);
    if (i.status == "aborted" && i.min_entry)
      minimum_attempts.erase(i.condition + ":" + i.token);
  } else if (type == "fill") {
    auto trade = e.at("trade").get<std::string>();
    if (!trade_ids.insert(trade).second)
      return;
    auto &i = orders.at(e.at("id").get<std::string>());
    auto sh = e.at("shares").get<Units>(), cost = e.at("cost").get<Units>(),
         fee = e.at("fee").get<Units>();
    auto &p = positions[i.token];
    p.token = i.token;
    p.condition = i.condition;
    p.shares = checked_money(static_cast<__int128>(p.shares) + sh);
    p.cost = checked_money(static_cast<__int128>(p.cost) + cost);
    p.fees = checked_money(static_cast<__int128>(p.fees) + fee);
    cash = checked_money(static_cast<__int128>(cash) - cost - fee);
    fees = checked_money(static_cast<__int128>(fees) + fee);
    i.booked_shares = checked_money(static_cast<__int128>(i.booked_shares) + sh);
    i.booked_cost = checked_money(static_cast<__int128>(i.booked_cost) + cost);
    i.booked_fee = checked_money(static_cast<__int128>(i.booked_fee) + fee);
    reindex(i.id);
  } else if (type == "settle") {
    auto condition = e.at("condition").get<std::string>();
    if (!settled_conditions.insert(condition).second)
      return;
    // What the resting orders' own fills made or lost in this round.
    for (const auto &[id, o] : orders) {
      (void)id;
      if (!o.rest || o.condition != condition || o.booked_shares <= 0)
        continue;
      auto paid = e.at("payouts").find(o.token);
      if (paid == e.at("payouts").end())
        continue;
      auto pnl = static_cast<__int128>(multiply(o.booked_shares, paid->get<Units>())) -
                 o.booked_cost - o.booked_fee;
      rest_realized = checked_money(static_cast<__int128>(rest_realized) + pnl);
    }
    for (auto it = e.at("payouts").begin(); it != e.at("payouts").end(); ++it) {
      auto p = positions.find(it.key());
      if (p == positions.end() || p->second.settled)
        continue;
      auto &v = p->second;
      if (v.condition != condition)
        throw std::runtime_error("settlement history contains another condition's position");
      Units payout = multiply(v.shares, it.value().get<Units>());
      cash = checked_money(static_cast<__int128>(cash) + payout);
      auto pnl = checked_money(static_cast<__int128>(payout) - v.cost - v.fees);
      realized = checked_money(static_cast<__int128>(realized) + pnl);
      if (pnl < 0)
        canary.settled_loss = checked_money(static_cast<__int128>(canary.settled_loss) - pnl);
      v.settled = true;
    }
    high_water = std::max(high_water, checked_money(static_cast<__int128>(reference_cash) + realized));
  } else if (type == "risk") {
    reference_cash = e.at("reference_cash");
    high_water = e.at("high_water");
    risk_epoch = e.at("epoch");
    dd_pause_until = e.at("pause_until");
  } else if (type == "release_slot") {
    auto &i = orders.at(e.at("id").get<std::string>());
    if (i.fill_slot && i.matched_shares == 0 && i.booked_shares == 0) {
      i.fill_slot = false;
      if (i.canary_slot_owned && i.canary_session == canary.started && canary.fills > 0)
        --canary.fills;
    }
  } else if (type == "restore_slot") {
    auto &i = orders.at(e.at("id").get<std::string>());
    if (!i.fill_slot && i.canary_slot_owned &&
        (i.matched_shares > 0 || i.booked_shares > 0)) {
      i.fill_slot = true;
      if (i.canary_session == canary.started) {
        if (canary.fills == std::numeric_limits<Units>::max())
          throw std::overflow_error("canary fill count overflow");
        ++canary.fills;
      }
    }
  } else if (type == "release_spend") {
    auto &i = orders.at(e.at("id").get<std::string>());
    if (i.canary_slot_owned && !i.canary_spend_released &&
        i.status == "no_match" && i.matched_shares == 0 && i.booked_shares == 0) {
      if (i.stake < 0)
        throw std::runtime_error("negative authorized canary spend");
      i.canary_spend_released = true;
      if (i.canary_session == canary.started)
        canary.spend = checked_money(std::max<__int128>(0, static_cast<__int128>(canary.spend) - i.stake));
    }
  } else if (type == "restore_spend") {
    auto &i = orders.at(e.at("id").get<std::string>());
    if (i.canary_slot_owned && i.canary_spend_released &&
        (i.matched_shares > 0 || i.booked_shares > 0)) {
      if (i.stake < 0)
        throw std::runtime_error("negative authorized canary spend");
      if (i.canary_session == canary.started)
        canary.spend = checked_money(static_cast<__int128>(canary.spend) + i.stake);
      i.canary_spend_released = false;
    }
  } else if (type == "canary")
    canary.load(e.at("state"));
  else
    throw std::runtime_error("unknown native ledger event");
}
void Ledger::register_intent(const Intent &i, const SignedOrder *signed_order,
                             const CanaryState &c) {
  if (orders.count(i.id))
    throw std::runtime_error("duplicate order intent");
  if (orders.size() >= 100000)
    throw std::runtime_error("native order capacity reached");
  Json event{
      {"type", "intent"}, {"intent", intent_json(i)}, {"canary", c.json()}};
  if (signed_order) {
    // The request envelope's owner is an API credential. Persist the signed
    // order and a wire hash, not the credential-bearing envelope.
    event["signed_order"] = Json::parse(signed_order->body).at("order");
    event["wire_sha256"] = sha256(signed_order->body);
  }
  commit(event);
}
void Ledger::status(std::string_view id, std::string_view s, Units shares,
                    Units cost) {
  if (!orders.count(std::string(id)))
    throw std::runtime_error("unauthorized order status");
  if (shares < 0 || cost < 0)
    throw std::invalid_argument("negative order match totals");
  commit({{"type", "status"},
          {"id", id},
          {"status", s},
          {"shares", shares},
          {"cost", cost}});
  restore_fill_slot(id);
}
bool Ledger::fill(std::string_view trade, std::string_view id, Units shares,
                  Units cost, Units fee) {
  if (trade_ids.count(std::string(trade)))
    return false;
  auto it = orders.find(std::string(id));
  if (it == orders.end() || shares <= 0 || cost < 0 || fee < 0)
    throw std::runtime_error("unauthorized or invalid fill");
  if (settled_conditions.count(it->second.condition))
    throw std::runtime_error("new fill arrived after recorded settlement");
  if (trade_ids.size() >= 200000)
    throw std::runtime_error("native trade capacity reached");
  if (static_cast<__int128>(cost) > static_cast<__int128>(multiply(shares, it->second.limit)) + 1)
    throw std::runtime_error("fill exceeds signed price cap");
  if (static_cast<__int128>(it->second.booked_cost) + cost > static_cast<__int128>(it->second.stake) + 1)
    throw std::runtime_error("fill total exceeds signed collateral");
  if (it->second.matched_cost > 0 &&
      static_cast<__int128>(it->second.booked_cost) + cost > static_cast<__int128>(it->second.matched_cost) + 1)
    throw std::runtime_error("fill cost exceeds reply total");
  const auto &intent = it->second;
  checked_money(static_cast<__int128>(intent.booked_shares) + shares);
  checked_money(static_cast<__int128>(intent.booked_cost) + cost);
  checked_money(static_cast<__int128>(intent.booked_fee) + fee);
  checked_money(static_cast<__int128>(intent.booked_cost) + intent.booked_fee + cost + fee);
  checked_money(static_cast<__int128>(cost) + fee);
  auto pos = positions.find(intent.token);
  if (pos != positions.end()) {
    if (pos->second.condition != intent.condition || pos->second.settled)
      throw std::runtime_error("fill position identity or settlement disagrees");
    checked_money(static_cast<__int128>(pos->second.shares) + shares);
    checked_money(static_cast<__int128>(pos->second.cost) + cost);
    checked_money(static_cast<__int128>(pos->second.fees) + fee);
    checked_money(static_cast<__int128>(pos->second.cost) + pos->second.fees + cost + fee);
  }
  checked_money(static_cast<__int128>(cash) - cost - fee);
  checked_money(static_cast<__int128>(fees) + fee);
  commit({{"type", "fill"},
          {"trade", trade},
          {"id", id},
          {"shares", shares},
          {"cost", cost},
          {"fee", fee}});
  restore_fill_slot(id);
  return true;
}
bool Ledger::settle(std::string_view condition,
                    const std::map<std::string, Units> &payouts) {
  for (const auto &[token, payout] : payouts) {
    auto position = positions.find(token);
    if (payout < 0 || payout > U ||
        (position != positions.end() && position->second.condition != condition))
      throw std::invalid_argument("settlement payout or condition is invalid");
  }
  if (settled_conditions.count(std::string(condition)))
    return false;
  for (auto &[id, i] : orders)
    if (i.condition == condition && (i.unresolved() || i.status == "matched"))
      return false;
  for (auto &[name, p] : positions)
    if (p.condition == condition && !p.settled && !payouts.count(name))
      return false;
  __int128 next_cash = cash, next_realized = realized, next_loss = canary.settled_loss;
  for (const auto &[token, payout] : payouts) {
    auto pos = positions.find(token);
    if (pos == positions.end() || pos->second.settled) continue;
    auto value = multiply(pos->second.shares, payout);
    auto pnl = static_cast<__int128>(value) - pos->second.cost - pos->second.fees;
    next_cash += value;
    next_realized += pnl;
    if (pnl < 0) next_loss -= pnl;
    checked_money(next_cash);
    checked_money(next_realized);
    checked_money(next_loss);
  }
  checked_money(next_cash);
  checked_money(next_realized);
  checked_money(next_loss);
  checked_money(static_cast<__int128>(reference_cash) + next_realized);
  commit({{"type", "settle"}, {"condition", condition}, {"payouts", payouts}});
  return true;
}
void Ledger::risk_rebase(Units equity, double pause_until) {
  if (equity < 0)
    equity = 0;
  commit({{"type", "risk"},
          {"reference_cash", checked_money(static_cast<__int128>(equity) - realized)},
          {"high_water", equity},
          {"epoch", wall_s()},
          {"pause_until", pause_until}});
}
void Ledger::drawdown_pause(double deadline) {
  commit({{"type", "risk"},
          {"reference_cash", reference_cash},
          {"high_water", high_water},
          {"epoch", risk_epoch},
          {"pause_until", deadline}});
}
void Ledger::set_canary(const CanaryState &s) {
  commit({{"type", "canary"}, {"state", s.json()}});
}
void Ledger::release_fill_slot(std::string_view id) {
  auto it = orders.find(std::string(id));
  if (it == orders.end() || !it->second.fill_slot ||
      it->second.matched_shares > 0 || it->second.booked_shares > 0)
    return;
  commit({{"type", "release_slot"}, {"id", id}});
}
void Ledger::release_missed_spend(std::string_view id) {
  const auto &i = orders.at(std::string(id));
  if (!i.canary_slot_owned || i.canary_spend_released ||
      i.status != "no_match" || i.matched_shares > 0 || i.booked_shares > 0)
    return;
  if (i.stake < 0)
    throw std::invalid_argument("negative authorized canary spend");
  commit({{"type", "release_spend"}, {"id", id}});
}
void Ledger::restore_fill_slot(std::string_view id) {
  const auto &i = orders.at(std::string(id));
  if (i.canary_slot_owned && !i.fill_slot &&
      (i.matched_shares > 0 || i.booked_shares > 0)) {
    if (i.canary_session == canary.started && canary.fills == std::numeric_limits<Units>::max())
      throw std::overflow_error("canary fill count overflow");
    commit({{"type", "restore_slot"}, {"id", id}});
  }
  if (i.canary_slot_owned && i.canary_spend_released &&
      (i.matched_shares > 0 || i.booked_shares > 0)) {
    if (i.stake < 0)
      throw std::invalid_argument("negative authorized canary spend");
    if (i.canary_session == canary.started)
      checked_money(static_cast<__int128>(canary.spend) + i.stake);
    commit({{"type", "restore_spend"}, {"id", id}});
  }
}
Units Ledger::holding(std::string_view token_id) const {
  auto i = positions.find(std::string(token_id));
  return i == positions.end() || i->second.settled ? 0 : i->second.shares;
}
void Ledger::reindex(const std::string &id) {
  auto &i = orders.at(id);
  // Independent HTTP, websocket and REST updates can arrive out of order.
  // A zero-fill terminal result must not release known execution exposure.
  // Also normalize snapshots written before this invariant was enforced.
  if ((i.matched_shares > 0 || i.booked_shares > 0) &&
      (i.status == "no_match" || i.status == "refused" ||
       i.status == "cancelled" || i.status == "aborted"))
    i.status = "matched";
  if (i.status == "confirmed" && i.matched_shares > i.booked_shares)
    i.status = "matched";
  if (i.status == "matched" &&
      (i.matched_shares > 0 || i.booked_shares > 0) &&
      i.booked_shares >= i.matched_shares)
    i.status = "confirmed";
  if (i.unresolved() || i.status == "matched")
    pending_[i.token].insert(id);
  else
    pending_[i.token].erase(id);
  if ((i.matched_shares > 0 || i.booked_shares > 0) &&
      counted_orders_.insert(id).second)
    ++round_fills_[i.condition];
}
int Ledger::round_fills(std::string_view condition) const {
  auto i = round_fills_.find(std::string(condition));
  return i == round_fills_.end() ? 0 : i->second;
}
Units Ledger::risk_shares(std::string_view t) const {
  __int128 held = holding(t);
  auto p = pending_.find(std::string(t));
  if (p != pending_.end())
    for (auto &id : p->second) {
      auto &i = orders.at(id);
      held += std::max<Units>(0, std::max(i.target, i.matched_shares) -
                                     i.booked_shares);
    }
  return checked_money(held);
}
Units Ledger::reserved() const {
  __int128 total = 0;
  for (auto &[t, ids] : pending_)
    for (auto &id : ids) {
      auto &i = orders.at(id);
      total += std::max<__int128>(0, static_cast<__int128>(i.reserve) - i.booked_cost - i.booked_fee);
    }
  return checked_money(total);
}
Units Ledger::open_cost() const {
  __int128 total = 0;
  for (auto &[name, p] : positions)
    if (!p.settled)
      total += static_cast<__int128>(p.cost) + p.fees;
  return checked_money(total + reserved());
}
bool Ledger::awaiting(std::string_view t) const {
  auto i = pending_.find(std::string(t));
  return i != pending_.end() && !i->second.empty();
}
Json Ledger::json() const {
  Json p = Json::array(), o = Json::array();
  for (auto &[name, v] : positions)
    p.push_back({{"condition", v.condition},
                 {"token", v.token},
                 {"shares", v.shares},
                 {"cost", v.cost},
                 {"fees", v.fees},
                 {"settled", v.settled}});
  for (auto &[id, i] : orders)
    o.push_back(intent_json(i));
  Json snapshot{{"format", "sniper-native-ledger-2"},
          {"sequence", wal_.sequence()},
          {"cash", cash},
          {"reference_cash", reference_cash},
          {"high_water", high_water},
          {"realized", realized},
          {"rest_realized", rest_realized},
          {"fees", fees},
          {"risk_epoch", risk_epoch},
          {"dd_pause_until", dd_pause_until},
          {"positions", p},
          {"orders", o},
          {"trade_ids", trade_ids},
          {"settled_conditions", settled_conditions},
          {"minimum_attempts", minimum_attempts},
          {"canary", canary.json()}};
  snapshot["sha256"] = sha256(snapshot.dump());
  return snapshot;
}
void Ledger::snapshot() { atomic_file(path_, json().dump()); }
} // namespace sniper
