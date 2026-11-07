#include "sniper/book.hpp"
#include "sniper/pyrt.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace sniper {
namespace {
double sys_mono() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
double sys_wall() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
std::string json_str(const Json &v) {
  // Python str(c.get("asset_id") or "")
  if (v.is_null())
    return "";
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_boolean())
    return v.get<bool>() ? "True" : "";
  if (v.is_number_integer() || v.is_number_unsigned())
    return v.get<long long>() == 0 ? "" : v.dump();
  if (v.is_number_float())
    return v.get<double>() == 0 ? "" : py::repr(v.get<double>());
  return v.empty() ? "" : v.dump();
}
bool truthy(const Json &v) {
  if (v.is_null())
    return false;
  if (v.is_boolean())
    return v.get<bool>();
  if (v.is_string() || v.is_array() || v.is_object())
    return !v.empty();
  if (v.is_number_float())
    return v.get<double>() != 0;
  if (v.is_number())
    return v.get<long long>() != 0;
  return true;
}
const Json &get(const Json &o, const char *key) {
  static const Json null;
  if (!o.is_object())
    return null;
  auto it = o.find(key);
  return it == o.end() ? null : *it;
}
// feeds/book._to_map
std::pair<std::shared_ptr<LevelMap>, bool> to_map(const Json &levels) {
  auto out = std::make_shared<LevelMap>();
  if (!levels.is_array())
    return {out, false};
  for (auto &lv : levels) {
    std::optional<double> p, s;
    if (lv.is_object()) {
      const Json &jp = get(lv, "price"), &js = get(lv, "size");
      if (jp.is_boolean() || js.is_boolean())
        return {std::make_shared<LevelMap>(), false};
      p = py_float(jp);
      s = py_float(js);
    } else if (lv.is_array()) {
      if (lv.size() < 2)
        return {std::make_shared<LevelMap>(), false};
      if (lv[0].is_boolean() || lv[1].is_boolean())
        return {std::make_shared<LevelMap>(), false};
      p = py_float(lv[0]);
      s = py_float(lv[1]);
    } else
      return {std::make_shared<LevelMap>(), false};
    if (!p || !s)
      return {std::make_shared<LevelMap>(), false};
    if (std::isfinite(*p) && std::isfinite(*s) && 0 < *p && *p < 1 &&
        *s > 0) {
      auto &slot = (*out)[*p];
      slot = slot + *s;
      if (!std::isfinite(slot))
        return {std::make_shared<LevelMap>(), false};
    } else
      return {std::make_shared<LevelMap>(), false};
  }
  return {out, true};
}
std::optional<long long> max_opt(std::initializer_list<std::optional<long long>> v) {
  std::optional<long long> r;
  for (auto &x : v)
    if (x && (!r || *x > *r))
      r = x;
  return r;
}
bool supported_tick(double tick) {
  return tick == 0.1 || tick == 0.01 || tick == 0.005 ||
         tick == 0.0025 || tick == 0.001 || tick == 0.0001;
}
} // namespace

std::optional<long long> py_int(const Json &v) {
  if (v.is_boolean() || v.is_null())
    return std::nullopt;
  if (v.is_number_unsigned()) {
    auto u = v.get<unsigned long long>();
    if (u > static_cast<unsigned long long>(LLONG_MAX))
      return std::nullopt;
    return static_cast<long long>(u);
  }
  if (v.is_number_integer())
    return v.get<long long>();
  if (v.is_number_float()) {
    double d = v.get<double>();
    if (!std::isfinite(d) || std::abs(d) >= 9.2e18)
      return std::nullopt;
    return static_cast<long long>(std::trunc(d));
  }
  if (!v.is_string())
    return std::nullopt;
  std::string s = v.get<std::string>();
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  s = s.substr(a, b - a);
  if (s.empty())
    return std::nullopt;
  std::size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = s[0] == '-';
    i = 1;
  }
  if (i >= s.size())
    return std::nullopt;
  unsigned long long acc = 0;
  const auto limit = static_cast<unsigned long long>(LLONG_MAX) + (neg ? 1 : 0);
  for (; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '_' && i > 0 && std::isdigit(static_cast<unsigned char>(s[i - 1])) &&
        i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))
      continue;
    if (!std::isdigit(static_cast<unsigned char>(ch)))
      return std::nullopt;
    unsigned digit = static_cast<unsigned>(ch - '0');
    if (acc > (limit - digit) / 10)
      return std::nullopt;
    acc = acc * 10 + digit;
  }
  if (neg && acc == static_cast<unsigned long long>(LLONG_MAX) + 1)
    return LLONG_MIN;
  return neg ? -static_cast<long long>(acc) : static_cast<long long>(acc);
}

std::optional<double> py_float(const Json &v) {
  if (v.is_boolean() || v.is_null())
    return std::nullopt;
  if (v.is_number_float())
    return v.get<double>();
  if (v.is_number_unsigned())
    return static_cast<double>(v.get<unsigned long long>());
  if (v.is_number_integer())
    return static_cast<double>(v.get<long long>());
  if (!v.is_string())
    return std::nullopt;
  try {
    return py::parse_float(v.get<std::string>());
  } catch (const py::ValueError &) {
    return std::nullopt;
  }
}

std::optional<double> parse_exchange_ts(double raw) {
  if (!std::isfinite(raw) || raw <= 0)
    return std::nullopt;
  if (raw < 1e11)
    return raw;
  if (raw < 1e14)
    return raw / 1e3;
  if (raw < 1e17)
    return raw / 1e6;
  if (raw < 1e20)
    return raw / 1e9;
  return std::nullopt;
}

BookState::BookState(double stale, double history, Clock mono, Clock wall,
                     Clock venue_wall)
    : stale_after(stale), history_s(history), mono_(mono ? mono : sys_mono),
      wall_(wall ? wall : sys_wall),
      venue_wall_(venue_wall ? venue_wall : (wall ? wall : sys_wall)) {}

bool BookState::fresh_exchange_ts(std::optional<long long> ts) const {
  if (!ts)
    return false;
  auto secs = parse_exchange_ts(static_cast<double>(*ts));
  if (!secs)
    return false;
  double age = venue_wall_() - *secs;
  return -future_tolerance <= age && age <= max_quiet;
}

void BookState::note_ts(std::optional<long long> ts) {
  if (ts && (!ts_hwm || *ts > *ts_hwm))
    ts_hwm = ts;
}

void BookState::note_exchange_ts(const Json &raw) {
  auto ts = py_int(raw);
  if (!fresh_exchange_ts(ts))
    return;
  note_ts(ts);
}

void BookState::record(const std::string &token, std::optional<long long> ts,
                       bool gap, std::optional<double> mono,
                       std::optional<double> wall) {
  if (history_s <= 0)
    return;
  auto &h = hist_[token];
  std::optional<long long> prev;
  if (!h.empty())
    prev = h.back().key;
  std::string source = gap ? "gap" : meta_[token].source;
  std::optional<long long> key =
      (ts && !gap && source != "rest") ? ts : ts_hwm;
  if (!key)
    key = prev ? *prev : 0;
  if (prev && *key < *prev)
    key = prev;
  double m = (!mono || source == "rest") ? mono_() : *mono;
  double w = wall ? *wall : wall_();
  Entry e;
  e.key = *key;
  e.mono = m;
  e.wall = w;
  e.source = source;
  if (!gap) {
    auto &mt = meta_[token];
    e.ts = ts;
    e.bids = bids_[token];
    e.asks = asks_[token];
    e.tick = mt.tick;
    e.receipt = mt.updated;
  }
  if (h.size() >= 20000)
    h.pop_front();
  h.push_back(std::move(e));
  double cutoff = m - history_s;
  while (h.size() > 1 && h[1].mono < cutoff)
    h.pop_front();
}

BookView BookState::entry_view(const std::string &token, const Entry &e) const {
  BookView v;
  v.token = token;
  auto g = generation_.find(token);
  v.generation = g == generation_.end() ? 0 : g->second;
  v.status = LIVE;
  for (auto it = e.bids->rbegin(); it != e.bids->rend(); ++it)
    v.bids.emplace_back(it->first, it->second);
  for (auto &[p, s] : *e.asks)
    v.asks.emplace_back(p, s);
  v.updated_mono = e.receipt;
  v.exchange_ts_ms = e.ts;
  v.tick_size = e.tick;
  v.source = e.source;
  return v;
}

std::pair<std::optional<BookView>, ViewAtInfo>
BookState::view_at(const std::string &token, double venue_ms,
                   bool allow_quiet) const {
  ViewAtInfo info;
  info.hwm_ms = ts_hwm;
  if (!ts_hwm || static_cast<double>(*ts_hwm) <= venue_ms) {
    if (!(allow_quiet && ts_hwm && connected && active_.count(token) &&
          status_locked(token) == LIVE)) {
      info.why = "the feed has not yet passed that time";
      return {std::nullopt, info};
    }
    info.evidence = "quiet";
  }
  auto h = hist_.find(token);
  if (h == hist_.end() || h->second.empty()) {
    info.why = "no book history for this token";
    return {std::nullopt, info};
  }
  const Entry *found = nullptr;
  for (auto it = h->second.rbegin(); it != h->second.rend(); ++it)
    if (static_cast<double>(it->key) <= venue_ms) {
      found = &*it;
      break;
    }
  if (!found) {
    info.why = "the book history does not reach back that far";
    return {std::nullopt, info};
  }
  if (!found->bids) {
    info.why = "feed gap (disconnect or resync) at that time";
    return {std::nullopt, info};
  }
  if (found->source == "rest") {
    info.why = "REST snapshot is not stream-ordered history";
    return {std::nullopt, info};
  }
  if (!found->ts) {
    info.why = "snapshot has no exchange timestamp";
    return {std::nullopt, info};
  }
  info.book_ts = found->ts;
  info.applied_wall = found->wall;
  return {entry_view(token, *found), info};
}

std::optional<BookView> BookState::view_at_mono(const std::string &token,
                                                double mono) const {
  auto h = hist_.find(token);
  if (h == hist_.end())
    return std::nullopt;
  for (auto it = h->second.rbegin(); it != h->second.rend(); ++it)
    if (it->mono <= mono) {
      if (!it->bids)
        return std::nullopt;
      return entry_view(token, *it);
    }
  return std::nullopt;
}

std::pair<std::vector<std::string>, std::vector<std::string>>
BookState::set_active(const std::vector<std::string> &tokens) {
  std::set<std::string> fresh;
  for (auto &t : tokens)
    if (!t.empty())
      fresh.insert(t);
  std::vector<std::string> added, removed;
  for (auto &t : fresh)
    if (!active_.count(t))
      added.push_back(t);
  for (auto &t : active_)
    if (!fresh.count(t))
      removed.push_back(t);
  for (auto &t : removed) {
    bids_.erase(t);
    asks_.erase(t);
    meta_.erase(t);
  }
  double cutoff = mono_() - history_s;
  for (auto it = hist_.begin(); it != hist_.end();) {
    if (!fresh.count(it->first) &&
        (it->second.empty() || it->second.back().mono < cutoff))
      it = hist_.erase(it);
    else
      ++it;
  }
  for (auto &t : added) {
    generation_[t] += 1;
    bids_[t] = std::make_shared<LevelMap>();
    asks_[t] = std::make_shared<LevelMap>();
    meta_[t] = Meta{};
  }
  active_ = fresh;
  return {added, removed};
}

std::vector<std::string> BookState::active() const {
  return {active_.begin(), active_.end()};
}

void BookState::desync_all(const std::string &reason) {
  for (auto &[t, m] : meta_) {
    generation_[t] += 1;
    m.synced = false;
    m.desync_reason = reason;
    record(t, std::nullopt, true);
  }
}

void BookState::desync(const std::vector<std::string> &tokens,
                       const std::string &reason) {
  std::set<std::string> seen;
  for (auto &token : tokens) {
    if (!seen.insert(token).second)
      continue;
    auto m = meta_.find(token);
    if (!active_.count(token) || m == meta_.end() || !m->second.synced)
      continue;
    m->second.synced = false;
    generation_[token] += 1;
    m->second.desync_reason = reason;
    record(token, std::nullopt, true);
    ++rejected_updates;
  }
}

void BookState::observe(const std::vector<std::string> &tokens,
                        std::optional<long long> ts) {
  std::set<std::string> set(tokens.begin(), tokens.end());
  for (auto &token : set) {
    if (!active_.count(token))
      continue;
    auto &m = meta_[token];
    m.receive_revision += 1;
    if (ts && (!m.seen_ts || *ts > *m.seen_ts))
      m.seen_ts = ts;
  }
}

std::optional<std::uint64_t>
BookState::subscription_gap(const std::string &token, std::uint64_t expected) {
  if (!active_.count(token) || generation_[token] != expected)
    return std::nullopt;
  observe({token});
  desync({token}, "snapshot resubscription");
  return generation_[token];
}

bool BookState::apply_snapshot(const std::string &token, const Json &bids,
                               const Json &asks, const SnapshotOptions &o) {
  if (!active_.count(token)) {
    ++dropped_inactive;
    return false;
  }
  auto &m = meta_[token];
  if (o.source != "ws" && o.source != "rest")
    throw std::invalid_argument("book snapshot source must be ws or rest");
  double now = mono_();
  double receipt = o.received_mono ? *o.received_mono : now;
  if (!std::isfinite(receipt) || receipt < 0 || receipt > now)
    throw std::invalid_argument("book snapshot receipt time is invalid");
  if (o.expected_generation && generation_[token] != *o.expected_generation)
    return false;
  if (o.expected_updates && m.updates != *o.expected_updates)
    return false;
  if (o.expected_receive_revision &&
      m.receive_revision != *o.expected_receive_revision)
    return false;
  auto incoming = py_int(o.ts_ms);
  if (!o.expected_receive_revision && o.source == "ws")
    observe({token}, fresh_exchange_ts(incoming) ? incoming : std::nullopt);
  if (o.only_if_unsynced && m.synced && status_locked(token) != STALE) {
    if (!o.refresh_if_older_than || !o.expected_generation ||
        !o.expected_updates || !m.updated ||
        now - *m.updated < *o.refresh_if_older_than)
      return false;
  }
  if (o.require_exchange_ts && !fresh_exchange_ts(incoming)) {
    desync({token}, "snapshot timestamp refused");
    return false;
  }
  if (o.tick && !supported_tick(*o.tick)) {
    desync({token}, "invalid snapshot tick size");
    return false;
  }
  auto previous = m.ts;
  if (m.seen_ts && (!previous || *m.seen_ts > *previous))
    previous = m.seen_ts;
  if (incoming && previous && *incoming < *previous)
    return false;
  auto [nb, vb] = to_map(bids);
  auto [na, va] = to_map(asks);
  if (!vb || !va) {
    desync({token}, "invalid snapshot");
    return false;
  }
  if (!nb->empty() && !na->empty() && nb->rbegin()->first >= na->begin()->first) {
    desync({token}, "crossed snapshot");
    return false;
  }
  bids_[token] = nb;
  asks_[token] = na;
  m.synced = true;
  m.updated = receipt;
  m.ts = incoming;
  m.source = o.source;
  m.hash = o.hash;
  m.updates += 1;
  if (o.tick)
    m.tick = o.tick;
  if (o.source == "ws")
    note_ts(incoming);
  record(token, incoming, false, m.updated);
  return true;
}

bool BookState::apply_price_changes(const Json &changes, const Json &ts_ms,
                                    const Json &hash, bool require_exchange_ts) {
  auto incoming = py_int(ts_ms);
  std::vector<std::string> all(active_.begin(), active_.end());
  if (!changes.is_array()) {
    observe(all);
    desync(all, "malformed price-change collection");
    return false;
  }
  std::vector<std::string> named;
  bool malformed = false;
  for (auto &c : changes) {
    if (c.is_object())
      named.push_back(json_str(get(c, "asset_id")));
    if (!c.is_object() || !truthy(get(c, "asset_id")))
      malformed = true;
  }
  auto fresh_ts = fresh_exchange_ts(incoming) ? incoming : std::nullopt;
  observe(malformed ? all : named, fresh_ts);
  if (malformed) {
    desync(all, "malformed event");
    return false;
  }
  if (require_exchange_ts && !fresh_exchange_ts(incoming)) {
    desync(named, "event timestamp refused");
    return false;
  }
  std::map<std::string, std::pair<LevelMap, LevelMap>> staged;
  std::vector<std::string> staged_order;
  std::map<std::string, std::string> token_hashes;
  std::vector<std::string> touched;
  bool skipped_pending = false;
  for (auto &change : changes) {
    std::string token = json_str(get(change, "asset_id"));
    if (!active_.count(token)) {
      ++dropped_inactive;
      continue;
    }
    auto &meta = meta_[token];
    if (!meta.synced) {
      skipped_pending = true;
      continue;
    }
    auto previous = max_opt({meta.seen_ts, meta.ts});
    if (incoming && previous && *incoming < *previous)
      continue;
    const Json &jp = get(change, "price"), &js = get(change, "size");
    if (jp.is_boolean() || js.is_boolean()) {
      desync(named, "invalid delta");
      return false;
    }
    auto price = py_float(jp), size = py_float(js);
    if (!price || !size) {
      desync(named, "invalid delta");
      return false;
    }
    std::string side = json_str(get(change, "side"));
    std::transform(side.begin(), side.end(), side.begin(),
                   [](unsigned char ch) { return std::toupper(ch); });
    if ((side != "BUY" && side != "BID" && side != "SELL" && side != "ASK") ||
        !std::isfinite(*price) || !std::isfinite(*size) || !(0 < *price) ||
        !(*price < 1) || *size < 0) {
      desync(named, "invalid delta");
      return false;
    }
    if (!staged.count(token)) {
      staged.emplace(token, std::make_pair(*bids_[token], *asks_[token]));
      staged_order.push_back(token);
    }
    auto &[sb, sa] = staged[token];
    auto &book = (side == "BUY" || side == "BID") ? sb : sa;
    if (*size == 0)
      book.erase(*price);
    else
      book[*price] = *size;
    if (std::find(touched.begin(), touched.end(), token) == touched.end())
      touched.push_back(token);
    const Json &h = get(change, "hash");
    if (truthy(h))
      token_hashes[token] = h.is_string() ? h.get<std::string>() : h.dump();
  }
  for (auto &[t, books] : staged) {
    auto &[sb, sa] = books;
    if (!sb.empty() && !sa.empty() && sb.rbegin()->first >= sa.begin()->first) {
      desync(staged_order, "delta crossed the book");
      return false;
    }
  }
  double now_mono = mono_(), now_wall = wall_();
  note_ts(incoming);
  for (auto &token : touched) {
    auto &[sb, sa] = staged[token];
    bids_[token] = std::make_shared<LevelMap>(std::move(sb));
    asks_[token] = std::make_shared<LevelMap>(std::move(sa));
    auto &meta = meta_[token];
    meta.updated = now_mono;
    meta.ts = incoming;
    meta.source = "ws";
    meta.updates += 1;
    auto th = token_hashes.find(token);
    if (th != token_hashes.end())
      meta.hash = th->second;
    else if (truthy(hash))
      meta.hash = hash.is_string() ? hash.get<std::string>() : hash.dump();
    record(token, incoming, false, now_mono, now_wall);
  }
  return !touched.empty() || skipped_pending;
}

bool BookState::set_tick_size(const std::string &token, const Json &tick,
                              const Json &ts_ms, bool require_exchange_ts) {
  auto mt = meta_.find(token);
  if (mt == meta_.end())
    return false;
  auto timestamp = py_int(ts_ms);
  if (require_exchange_ts && !fresh_exchange_ts(timestamp))
    return false;
  auto &meta = mt->second;
  auto previous = max_opt({meta.tick_ts, meta.seen_ts, meta.ts});
  if (timestamp && previous && *timestamp < *previous)
    return false;
  if (tick.is_boolean())
    return false;
  auto parsed = py_float(tick);
  if (!parsed || !supported_tick(*parsed)) {
    desync({token}, "unsupported venue tick size");
    return false;
  }
  observe({token});
  meta.tick = parsed;
  if (timestamp) {
    meta.tick_ts = timestamp;
    note_ts(timestamp);
  }
  if (meta.synced)
    record(token, timestamp ? timestamp : meta.ts);
  return true;
}

std::string BookState::status_locked(const std::string &token) const {
  if (!connected)
    return DISCONNECTED;
  auto m = meta_.find(token);
  if (m == meta_.end() || !m->second.synced || !m->second.updated)
    return UNSYNCED;
  return (mono_() - *m->second.updated) <= stale_after ? LIVE : STALE;
}

std::string BookState::status(const std::string &token) const {
  return status_locked(token);
}

BookView BookState::view(const std::string &token) const {
  BookView v;
  v.token = token;
  auto g = generation_.find(token);
  v.generation = g == generation_.end() ? 0 : g->second;
  if (!active_.count(token)) {
    v.status = DISCONNECTED;
    return v;
  }
  v.status = status_locked(token);
  auto b = bids_.find(token), a = asks_.find(token);
  if (b != bids_.end() && b->second)
    for (auto it = b->second->rbegin(); it != b->second->rend(); ++it)
      v.bids.emplace_back(it->first, it->second);
  if (a != asks_.end() && a->second)
    for (auto &[p, s] : *a->second)
      v.asks.emplace_back(p, s);
  auto m = meta_.find(token);
  if (m != meta_.end()) {
    v.updated_mono = m->second.updated;
    v.exchange_ts_ms = m->second.ts;
    v.tick_size = m->second.tick;
    v.hash = m->second.hash;
    v.updates = m->second.updates;
    v.receive_revision = m->second.receive_revision;
    v.source = m->second.source;
  }
  return v;
}

std::vector<std::string> BookState::needs_resync(bool include_stale) const {
  std::vector<std::string> out;
  for (auto &t : active_) {
    auto m = meta_.find(t);
    bool synced = m != meta_.end() && m->second.synced;
    if (!synced || (include_stale && status_locked(t) == STALE))
      out.push_back(t);
  }
  return out;
}

std::size_t BookState::history_entries() const {
  std::size_t n = 0;
  for (auto &[t, h] : hist_)
    n += h.size();
  return n;
}

bool dispatch_market_event(BookState &book, const Json &m) {
  std::string et = get(m, "event_type").is_string()
                       ? get(m, "event_type").get<std::string>()
                       : "";
  auto asset = [&]() {
    const Json &a = get(m, "asset_id");
    if (a.is_null())
      return std::string("None");
    if (a.is_string())
      return a.get<std::string>();
    return a.dump();
  };
  if (et == "book") {
    SnapshotOptions o;
    o.ts_ms = get(m, "timestamp");
    if (m.contains("tick_size")) {
      o.tick = py_float(get(m, "tick_size")).value_or(NAN);
    }
    const Json &h = get(m, "hash");
    if (!h.is_null())
      o.hash = h.is_string() ? h.get<std::string>() : h.dump();
    o.require_exchange_ts = true;
    return !book.apply_snapshot(asset(), get(m, "bids"), get(m, "asks"), o);
  }
  if (et == "price_change")
    return !book.apply_price_changes(get(m, "price_changes"),
                                     get(m, "timestamp"), get(m, "hash"), true);
  if (et == "tick_size_change") {
    book.set_tick_size(asset(), get(m, "new_tick_size"), get(m, "timestamp"),
                       !get(m, "timestamp").is_null());
    return false;
  }
  if (et == "last_trade_price") {
    std::string token = json_str(get(m, "asset_id"));
    if (!book.is_active(token)) {
      book.note_inactive_drop();
      return false;
    }
    const Json &jp = get(m, "price"), &jt = get(m, "timestamp");
    if (jp.is_boolean() || jt.is_boolean())
      return false;
    auto price = py_float(jp);
    std::optional<long long> ts;
    if (jt.is_string() || jt.is_number_integer() || jt.is_number_unsigned())
      ts = py_int(jt);
    else if (jt.is_number_float() && std::isfinite(jt.get<double>()))
      ts = py_int(jt);
    if (!price || !ts)
      return false;
    if (!std::isfinite(*price) || !(0 < *price && *price < 1) ||
        !book.fresh_exchange_ts(ts))
      return false;
    book.note_exchange_ts(Json(*ts));
    return false;
  }
  if (et == "best_bid_ask") {
    if (book.is_active(json_str(get(m, "asset_id"))))
      book.note_exchange_ts(get(m, "timestamp"));
    return false;
  }
  return false;
}

int handle_market_frame(BookState &book, std::string_view raw) {
  std::string text(raw);
  auto a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
  if (a == std::string::npos)
    return 0;
  text = text.substr(a, b - a + 1);
  if (text == "PONG" || text == "pong")
    return 0;
  Json msg;
  try {
    msg = Json::parse(text);
  } catch (...) {
    return 0;
  }
  int refused = 0;
  if (msg.is_array()) {
    for (auto &item : msg)
      if (item.is_object() && dispatch_market_event(book, item))
        ++refused;
  } else if (msg.is_object() && dispatch_market_event(book, msg))
    ++refused;
  return refused;
}

void Freshness::note_trade(std::optional<double> event_ms, double recv_wall_ms,
                           double mono) {
  if (!event_ms || !std::isfinite(recv_wall_ms - *event_ms)) {
    ++trade_without_e;
    last_trade_mono.reset();
    last_excess.reset();
    return;
  }
  double lag = recv_wall_ms - *event_ms;
  while (!min_q.empty() && min_q.back().second >= lag)
    min_q.pop_back();
  min_q.emplace_back(mono, lag);
  double cut = mono - baseline_s;
  while (min_q.front().first < cut)
    min_q.pop_front();
  last_excess = lag - min_q.front().second;
  last_trade_mono = mono;
  ++trades;
}

std::string Freshness::why_not(double now) const {
  if (!enabled)
    return "";
  if (!connected)
    return name + " disconnected";
  if (backlog > backlog_max && !(mode == "age" && fresh_burst()))
    return name + " receive backlog";
  if (!last_trade_mono || (now - *last_trade_mono) * 1000.0 > unknown_ms)
    return name + " age unknown";
  if (last_excess && *last_excess > stale_ms)
    return name + " feed behind";
  return "";
}

// ---------------------------------------------------------------- discovery
std::optional<double> exchange_ms(const Json &j) {
  try {
    if (j.is_number())
      return number(j);
    if (!j.is_string())
      return {};
    auto s = j.get<std::string>();
    if (s.find('T') == s.npos) {
      double n = std::stod(s);
      return std::isfinite(n) ? std::optional(n) : std::nullopt;
    }
    if (s.size() < 19)
      return {};
    std::tm tm{};
    std::istringstream in(s.substr(0, 19));
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail())
      return {};
    double ms = static_cast<double>(timegm(&tm)) * 1000;
    std::size_t suffix = 19;
    if (suffix < s.size() && s[suffix] == '.') {
      auto first = ++suffix;
      while (suffix < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[suffix])))
        ++suffix;
      if (suffix == first)
        return {};
      ms += std::stod("0." + s.substr(first, suffix - first)) * 1000;
    }
    // timegm interprets the date and time as UTC; apply the ISO 8601 offset
    // separately. A timestamp without a zone retains the historical UTC default.
    if (suffix == s.size())
      return ms;
    if (s[suffix] == 'Z')
      return suffix + 1 == s.size() ? std::optional(ms) : std::nullopt;
    int sign = s[suffix] == '+' ? 1 : (s[suffix] == '-' ? -1 : 0);
    if (!sign)
      return {};
    ++suffix;
    auto two_digits = [&](int &value) {
      if (suffix + 2 > s.size() ||
          !std::isdigit(static_cast<unsigned char>(s[suffix])) ||
          !std::isdigit(static_cast<unsigned char>(s[suffix + 1])))
        return false;
      value = (s[suffix] - '0') * 10 + (s[suffix + 1] - '0');
      suffix += 2;
      return true;
    };
    int hours = 0, minutes = 0;
    if (!two_digits(hours))
      return {};
    if (suffix < s.size()) {
      if (s[suffix] == ':')
        ++suffix;
      if (!two_digits(minutes))
        return {};
    }
    if (suffix != s.size() || hours > 23 || minutes > 59)
      return {};
    ms -= sign * (hours * 3600 + minutes * 60) * 1000.0;
    return ms;
  } catch (...) {
    return {};
  }
}
static Json list(const Json &v) {
  if (v.is_array())
    return v;
  if (v.is_string())
    return Json::parse(v.get<std::string>());
  return Json::array();
}
std::optional<Round> parse_round(const Json &e, std::int64_t start) {
  try {
    std::string slug = "btc-updown-5m-" + std::to_string(start);
    if (!e.is_object() || e.value("slug", "") != slug ||
        !e.value("active", false) || e.value("closed", false))
      return {};
    std::optional<Round> found;
    for (auto &m : e.at("markets")) {
      if (!m.value("active", false) || m.value("closed", false) ||
          !m.value("acceptingOrders", false) ||
          !m.value("enableOrderBook", false))
        continue;
      // Both ID fields can be populated. This native signer and settlement
      // path support CTF v1 only; never select legacy IDs from a v2 market.
      if (m.value("version", "") != "v1")
        continue;
      auto cfg = m.at("cryptoMarketConfig");
      if (cfg.value("asset", "") != "btc" ||
          cfg.value("duration", "") != "5m" ||
          !cfg.value("twapEnabled", false) ||
          number(cfg.at("twapLookbackSeconds")) != 60)
        continue;
      auto s = exchange_ms(m.at("eventStartTime")),
           end = exchange_ms(m.at("endDate"));
      if (!s || !end || std::abs(*s / 1000 - start) > 1 ||
          std::abs(*end / 1000 - start - 300) > 1)
        continue;
      auto tokens = list(m.at("clobTokenIds")),
           outcomes = list(m.at("outcomes"));
      if (tokens.size() != 2 || outcomes.size() != 2)
        continue;
      Round r;
      r.condition = m.at("conditionId");
      if (!valid_hex(r.condition, 32))
        continue;
      r.start = start;
      r.end = start + 300;
      r.slug = slug;
      r.market_id = m.at("id").is_string() ? m.at("id").get<std::string>()
                                           : m.at("id").dump();
      for (int i = 0; i < 2; ++i) {
        auto label = outcomes[i].get<std::string>();
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (label == "up")
          r.up = tokens[i].get<std::string>();
        else if (label == "down")
          r.down = tokens[i].get<std::string>();
      }
      if (!valid_token(r.up) || !valid_token(r.down) || r.up == r.down)
        continue;
      if (e.contains("eventMetadata") &&
          e["eventMetadata"].contains("priceToBeat") &&
          !e["eventMetadata"]["priceToBeat"].is_null())
        r.ptb = number(e["eventMetadata"]["priceToBeat"]);
      if (found)
        return {};
      found = r;
    }
    return found;
  } catch (...) {
    return {};
  }
}
Rules parse_rules(const Json &m, const Json &b, std::string_view t) {
  Rules r;
  if (!m.is_object() || !m.contains("fd") || !m.at("fd").is_object())
    throw std::runtime_error("market fee rules not loaded");
  if (m.value("v", "") != "v1")
    throw std::runtime_error("unsupported market protocol version");
  auto condition = m.at("c").get<std::string>();
  if (!valid_hex(condition, 32) || !valid_token(t) || !b.is_object() ||
      b.value("asset_id", "") != t || b.value("market", "") != condition)
    throw std::runtime_error("market rules and book identity disagree");
  r.tick = as_units(m.at("mts"));
  r.rate = as_units(m.at("fd").at("r"));
  double exponent = number(m.at("fd").at("e"));
  if (exponent < 1 || exponent > 8 || exponent != std::floor(exponent))
    throw std::runtime_error("unsupported market fee exponent");
  r.exponent = static_cast<int>(exponent);
  // The CLOB omits nr for standard markets; the official SDK uses false.
  r.neg = m.value("nr", false);
  r.min_shares = as_units(b.at("min_order_size"));
  if (r.tick != as_units(b.at("tick_size")) ||
      r.min_shares != as_units(m.at("mos")) ||
      (b.contains("neg_risk") && b.at("neg_risk").get<bool>() != r.neg))
    throw std::runtime_error("market rules and book parameters disagree");
  bool exists = false;
  for (auto &v : m.at("t"))
    if (v.at("t") == t)
      exists = true;
  if (!exists || r.tick <= 0 || r.rate < 0 || r.rate > U || r.exponent < 1 ||
      r.exponent > 8 || r.min_shares <= 0)
    throw std::runtime_error("invalid market rules");
  market_amounts(U, r.tick, r.tick);
  r.loaded = true;
  r.generation = 1;
  return r;
}
} // namespace sniper
