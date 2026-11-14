#include "sniper/sniper.hpp"
#include "sniper/math.hpp"
#include "sniper/feeds.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sniper {
using py::D;
using py::Dec;
namespace {
constexpr double MIN_ORDER_USD = 1.0;
constexpr long long KELLY_MIN_SHARES = 5;
constexpr double ROUND_S = 300;
constexpr double BANKROLL_STALE_S = 30.0, BANKROLL_SETTLE_S = 2.0,
                 HYBRID_RISK_STALE_S = 30.0;

const Json &field(const Json &o, const char *key) {
  static const Json null;
  if (!o.is_object())
    return null;
  auto it = o.find(key);
  return it == o.end() ? null : *it;
}
// A round end as the hub holds it: an int when it is whole.
Json end_json(double end) {
  if (std::floor(end) == end && std::abs(end) < 9e15)
    return Json(static_cast<long long>(end));
  return Json(end);
}
using py::lower;
bool starts_with(const std::string &s, const std::string &p) {
  return s.rfind(p, 0) == 0;
}
std::string error_text(const std::exception &e) {
  if (dynamic_cast<const py::ValueError *>(&e))
    return std::string("ValueError: ") + e.what();
  if (dynamic_cast<const std::invalid_argument *>(&e))
    return std::string("ValueError: ") + e.what();
  return std::string("RuntimeError: ") + e.what();
}
Json factors_json(const hybrid::Factors &f) {
  Json j;
  j["volatility_scale"] = f.volatility_scale;
  j["drawdown"] = f.drawdown;
  j["drawdown_scale"] = f.drawdown_scale;
  j["confidence_scale"] = 1.0;
  j["confidence_status"] = "unvalidated_neutral";
  j["effective_fraction"] = f.effective_fraction;
  j["risk_budget_usd"] =
      f.risk_budget_usd ? Json(*f.risk_budget_usd) : Json(nullptr);
  j["hard_cap_enabled"] = f.hard_cap_enabled;
  return j;
}
// sniper_mode._dec
std::optional<double> cb_dec(const Json &v) {
  if (v.is_boolean() || !(v.is_string() || v.is_number()))
    return std::nullopt;
  auto x = py_float(v);
  if (!x || !std::isfinite(*x) || *x <= 0)
    return std::nullopt;
  return x;
}
} // namespace

std::optional<double> iso_ms(const Json &v) {
  if (!v.is_string())
    return std::nullopt;
  std::string s = v.get<std::string>();
  for (std::size_t p; (p = s.find('Z')) != std::string::npos;)
    s.replace(p, 1, "+00:00");
  auto digits = [&](std::size_t at, std::size_t n, int &out) {
    if (at + n > s.size())
      return false;
    out = 0;
    for (std::size_t i = at; i < at + n; ++i) {
      if (!std::isdigit(static_cast<unsigned char>(s[i])))
        return false;
      out = out * 10 + (s[i] - '0');
    }
    return true;
  };
  int Y, M, Dd, h = 0, mi = 0, se = 0;
  if (!digits(0, 4, Y) || s.size() < 10 || s[4] != '-' || !digits(5, 2, M) ||
      s[7] != '-' || !digits(8, 2, Dd))
    return std::nullopt;
  std::size_t i = 10;
  long long micros = 0;
  if (i < s.size()) {
    if (s[i] != 'T' && s[i] != ' ')
      return std::nullopt;
    ++i;
    if (!digits(i, 2, h))
      return std::nullopt;
    i += 2;
    if (i < s.size() && s[i] == ':') {
      if (!digits(i + 1, 2, mi))
        return std::nullopt;
      i += 3;
      if (i < s.size() && s[i] == ':') {
        if (!digits(i + 1, 2, se))
          return std::nullopt;
        i += 3;
        if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
          ++i;
          int nd = 0;
          long long frac = 0;
          while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            if (nd < 6) {
              frac = frac * 10 + (s[i] - '0');
              ++nd;
            }
            ++i;
          }
          if (nd == 0)
            return std::nullopt;
          while (nd < 6) {
            frac *= 10;
            ++nd;
          }
          micros = frac;
        }
      }
    }
  }
  if (i >= s.size())
    return std::nullopt; // naive: the reference returns None
  int sign = s[i] == '+' ? 1 : (s[i] == '-' ? -1 : 0);
  if (!sign)
    return std::nullopt;
  ++i;
  int oh = 0, om = 0;
  if (!digits(i, 2, oh))
    return std::nullopt;
  i += 2;
  if (i < s.size() && s[i] == ':')
    ++i;
  if (i < s.size()) {
    if (!digits(i, 2, om))
      return std::nullopt;
    i += 2;
  }
  if (i != s.size())
    return std::nullopt;
  const std::chrono::year_month_day date{
      std::chrono::year{Y}, std::chrono::month{static_cast<unsigned>(M)},
      std::chrono::day{static_cast<unsigned>(Dd)}};
  if (Y < 1 || !date.ok() || h > 23 || mi > 59 || se > 59 || oh > 23 || om > 59)
    return std::nullopt;
  // days from civil (Howard Hinnant)
  int y = Y - (M <= 2);
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = static_cast<unsigned>(y - era * 400);
  unsigned doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + Dd - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long long days = static_cast<long long>(era) * 146097 + doe - 719468;
  long long secs = days * 86400 + h * 3600 + mi * 60 + se -
                   sign * (oh * 3600 + om * 60);
  long long total_us = secs * 1000000 + micros;
  return (static_cast<double>(total_us) / 1e6) * 1000.0;
}

std::string decision_id(double wall, const std::string &side) {
  return py::fixed(wall, 3) + "-" + side;
}

double ioc_spend(const PxLevels &levels, double target, double ask,
                 double cap) {
  double need = target, cost = 0.0;
  for (auto &[price, size] : levels) {
    if (price > cap + 1e-9 || need <= 1e-9)
      break;
    double take = std::min(need, size);
    cost += take * price;
    need -= take;
  }
  cost += std::max(0.0, need) * ask;
  return std::ceil(cost * 100.0 - 1e-6) / 100.0;
}

std::string paper_outcome(bool filled, const std::string &reason,
                          const Json &row, bool history) {
  if (filled)
    return "FILLED";
  std::string r = lower(reason);
  const Json &arrival = field(row, "arrival");
  auto has = [&](const char *t) { return r.find(t) != std::string::npos; };
  if (has("unknown book state"))
    return "UNKNOWN_BOOK_STATE";
  if (has("makers pulled (live fill odds"))
    return "LIVE_ODDS_MISS";
  if (has("fok no-fill") || has("fak no-fill")) {
    const Json &ask = field(arrival, "best_ask");
    if (ask.is_null())
      return history ? "MISS_AT_DECISION" : "FOK_MISS_AT_WAKE";
    double cap = field(row, "cap").is_number() ? field(row, "cap").get<double>()
                                               : 0.0;
    return ask.get<double>() > cap + 1e-9 ? "PRICE_CAP_MISS"
                                          : "INSUFFICIENT_LIQUIDITY";
  }
  if (has("below venue minimum"))
    return "BELOW_VENUE_MINIMUM";
  if (has("stale") || has("has not changed for") || has("future-dated"))
    return "STALE_BOOK";
  if (has("spread"))
    return "SPREAD_TOO_WIDE";
  return "TECHNICAL_ERROR";
}

Sniper::Sniper(const SniperSettings &settings, SniperEnv &e)
    : s(settings), env(e), trigger(settings.k_bp, settings.window_ms),
      vol(settings.fair_vol_s) {
  cb_fresh.stale_ms = s.coinbase_stale_ms;
  cb_fresh.unknown_ms = s.coinbase_unknown_ms;
  cb_fresh.backlog_max = s.coinbase_backlog_max;
  cb_fresh.name = "coinbase";
  cb_fresh.mode = s.backlog_gate;
  cb_fresh.fresh_ms = s.backlog_fresh_ms;
  fire_feed_ = s.primary_feed;
  for (const auto &name : s.extra_feeds) {
    Extra f{name, Trigger(s.k_bp, s.window_ms), Freshness{}, std::nullopt};
    f.fresh.enabled = s.binance_freshness;
    f.fresh.stale_ms = s.binance_stale_ms;
    f.fresh.unknown_ms = s.binance_unknown_ms;
    f.fresh.backlog_max = s.binance_backlog_max;
    f.fresh.name = name;
    f.fresh.mode = s.backlog_gate;
    f.fresh.fresh_ms = s.backlog_fresh_ms;
    extra.push_back(std::move(f));
  }
}

// ------------------------------------------------------------ extra feeds
std::string Sniper::on_extra(std::size_t i, std::string_view raw, double t,
                             std::int64_t t_ns) {
  auto &f = extra.at(i);
  auto tick = feeds::parse(f.name, raw);
  if (tick.kind == feeds::Tick::Malformed) {
    ++n[f.name + " malformed"];
    return "malformed";
  }
  if (tick.kind == feeds::Tick::Trade) {
    // binance_spot: its trades carry the exchange's clock for the gate
    f.fresh.note_trade(tick.event_ms, env.wall() * 1000.0, t / 1000.0);
    ++n[f.name + " trades"];
    return "trade";
  }
  if (tick.kind != feeds::Tick::Book)
    return "other";
  if (f.name == "binance_fut") // its book ticker carries the exchange's clock
    f.fresh.note_trade(tick.event_ms, env.wall() * 1000.0, t / 1000.0);
  ++n[f.name + " ticks"];
  bool fresh_enough = f.fresh.why_not(t / 1000.0).empty();
  f.last_mid = tick.mid;
  signal_on(f.name, f.trigger, f.fresh, *tick.mid, t, t_ns);
  if (!fresh_enough) {
    f.trigger.q.values.clear();
    f.trigger.ref.reset();
  }
  return "tick";
}

void Sniper::on_extra_connect(std::size_t i) {
  auto &f = extra.at(i);
  f.trigger = Trigger(s.k_bp, s.window_ms);
  f.last_mid.reset();
  f.fresh.on_connect();
}

const Freshness &Sniper::freshness(const std::string &feed) const {
  for (const auto &f : extra)
    if (f.name == feed)
      return f.fresh;
  return cb_fresh;
}

std::optional<double> Sniper::mid_of(const std::string &feed) const {
  for (const auto &f : extra)
    if (f.name == feed)
      return f.last_mid;
  return last_mid;
}

const Trigger &Sniper::trigger_of(const std::string &feed) const {
  for (const auto &f : extra)
    if (f.name == feed)
      return f.trigger;
  return trigger;
}

// ------------------------------------------------------------ the feed
std::string Sniper::on_coinbase(std::string_view raw, double t,
                                std::int64_t t_ns) {
  Json d;
  try {
    d = Json::parse(raw);
  } catch (...) {
    ++n["coinbase malformed"];
    return "malformed";
  }
  if (!d.is_object()) {
    ++n["coinbase malformed"];
    return "malformed";
  }
  const Json &kind = field(d, "type");
  if (!(kind.is_string() && kind.get<std::string>() == "ticker")) {
    std::string k = kind.is_string() ? kind.get<std::string>() : "";
    if (k == "heartbeat") {
      ++n["coinbase heartbeats"];
      return "heartbeat";
    }
    if (k == "subscriptions") {
      std::map<std::string, std::set<std::string>> got;
      const Json &channels = field(d, "channels");
      if (channels.is_array())
        for (auto &ch : channels)
          if (ch.is_object() && field(ch, "product_ids").is_array()) {
            std::set<std::string> ids;
            for (auto &p : ch["product_ids"])
              if (p.is_string())
                ids.insert(p.get<std::string>());
            std::string name = field(ch, "name").is_string()
                                   ? ch["name"].get<std::string>()
                                   : "";
            got[name] = ids;
          }
      std::vector<std::string> missing;
      for (const char *c : {"ticker", "heartbeat"})
        if (!got.count(c) || !got[c].count("BTC-USD"))
          missing.push_back(c);
      if (!missing.empty()) {
        std::string m = missing[0];
        for (std::size_t i = 1; i < missing.size(); ++i)
          m += "/" + missing[i];
        throw std::runtime_error("coinbase subscription missing " + m +
                                 " for BTC-USD");
      }
      return "subscribed";
    }
    if (k == "error") {
      std::string msg = field(d, "message").is_string()
                            ? d["message"].get<std::string>()
                            : field(d, "message").dump();
      std::string why = field(d, "reason").is_string()
                            ? d["reason"].get<std::string>()
                            : field(d, "reason").dump();
      throw std::runtime_error((msg + ": " + why).substr(0, 160));
    }
    return "other";
  }
  std::optional<double> mid;
  if (field(d, "product_id").is_string() &&
      d["product_id"].get<std::string>() == "BTC-USD") {
    auto bid = cb_dec(field(d, "best_bid")), ask = cb_dec(field(d, "best_ask"));
    if (bid && ask && !(*bid > *ask))
      mid = (*bid + *ask) / 2.0;
  }
  if (!mid) {
    ++n["coinbase malformed"];
    return "malformed";
  }
  const Json &seq = field(d, "sequence");
  bool many = s.coinbase_conns > 1 && s.primary_feed == "coinbase";
  if ((seq.is_number_integer() || seq.is_number_unsigned()) &&
      !seq.is_boolean()) {
    long long q = seq.is_number_unsigned()
                      ? static_cast<long long>(seq.get<unsigned long long>())
                      : seq.get<long long>();
    if (coinbase_seq && q <= *coinbase_seq) {
      ++n[many ? "coinbase already heard" : "coinbase out of order"];
      return "late";
    }
    coinbase_seq = q;
  } else if (many) {
    ++n["coinbase malformed"];
    return "malformed";
  }
  double wall = env.wall();
  cb_fresh.note_trade(iso_ms(field(d, "time")), wall * 1000.0, t / 1000.0);
  ++n["coinbase ticks"];
  bool fresh_enough = cb_fresh.why_not(t / 1000.0).empty();
  last_mid = mid;
  if (fresh_enough) {
    vol.observe(t / 1000.0, *mid);
    note_primary_mid(*mid, wall, t / 1000.0);
  }
  signal(*mid, t, t_ns);
  if (!fresh_enough) {
    trigger.q.values.clear();
    trigger.ref.reset();
  }
  return "tick";
}

void Sniper::signal(double mid, double t, std::int64_t t_ns) {
  signal_on(s.primary_feed, trigger, cb_fresh, mid, t, t_ns);
}

void Sniper::signal_on(const std::string &feed, Trigger &trig, Freshness &fresh,
                       double mid, double t, std::int64_t t_ns) {
  if (closing)
    return;
  auto [direction, r] = trig.update(t, mid);
  if (!direction)
    return;
  const bool primary = feed == s.primary_feed;
  const std::string tag = primary ? "" : " (" + feed + ")";
  ++n[primary ? std::string("signals") : "signals " + feed];
  std::string why = fresh.why_not(t / 1000.0);
  std::string side = *direction > 0 ? "UP" : "DOWN";
  ++signals_by_side[idx(side)];
  if (!why.empty()) {
    // measured on ticks that cannot be shown current: the reference goes too,
    // so the rule restarts on fresh ticks only
    Json window = window_fields(trig, t);
    trig.q.values.clear();
    double wall = env.wall();
    Json row;
    row["wall"] = wall;
    row["id"] = decision_id(wall, side);
    row["side"] = side;
    row["bp"] = py::round(r, 3);
    row["feed"] = feed;
    for (auto &[k, v] : window.items())
      row[k] = v;
    row[feed + "_backlog"] = fresh.backlog;
    row[feed + "_excess_ms"] = fresh.last_excess
                                   ? Json(py::round(*fresh.last_excess, 1))
                                   : Json(nullptr);
    skip(why, row);
    return;
  }
  int i = idx(side);
  if (t - last_fire_by_side[i] < s.cooldown_ms) {
    ++n["skip: cooldown" + tag]; // this side only: the other is not blocked
    return;
  }
  if (inflight[i]) {
    ++n["skip: previous order in flight" + tag];
    return;
  }
  inflight[i] = InFlight{};
  t1_mid_ = mid;
  fire_feed_ = feed;
  auto plan = fire_now(side, r, t, t_ns);
  fire_feed_ = s.primary_feed;
  t1_mid_.reset();
  if (plan) {
    last_fire_by_side[i] = env.mono() * 1000.0;
    ++n["fired " + feed];
  }
}

Json Sniper::window_fields(const Trigger &trig, double t_ms) const {
  Json out = Json::object();
  if (!trig.ref || trig.ref->first > t_ms)
    return out;
  out["window_ms"] = py::round(t_ms - trig.ref->first, 1);
  out["ref_mid"] = trig.ref->second;
  return out;
}

std::vector<std::shared_ptr<Plan>> Sniper::take_dispatched() {
  auto out = std::move(dispatched_);
  dispatched_.clear();
  return out;
}

// ------------------------------------------------------------ skips
std::nullptr_t Sniper::skip(const std::string &why, Json &row) {
  std::uint64_t count = ++n["skip: " + why];
  row["result"] = "skipped";
  row["reason"] = why;
  std::string key = why + "\x1f" + field(row, "round_end").dump() + "\x1f" +
                    field(row, "side").dump();
  double now = env.mono();
  auto prev = skip_logged_.find(key);
  if (prev != skip_logged_.end() && now - prev->second < 1.0)
    return nullptr;
  if (prev == skip_logged_.end())
    skip_order_.push_back(key);
  skip_logged_[key] = now;
  while (skip_logged_.size() > 128) {
    skip_logged_.erase(skip_order_.front());
    skip_order_.erase(skip_order_.begin());
  }
  row["skip_count"] = count;
  write(row);
  return nullptr;
}

// ------------------------------------------------------------ limits
std::pair<double, Json> Sniper::latency_plan() {
  if (s.fixed_latency)
    return {s.latency_ms, Json{{"latency_mode", "fixed"}}};
  double extra = env.live() ? 0.0 : s.paper_extra_ms;
  auto rtt = env.net_rtt();
  bool measured = !rtt.empty();
  double net = s.net_default_ms;
  if (measured) {
    std::sort(rtt.begin(), rtt.end());
    net = rtt[rtt.size() / 2] / 2.0;
  }
  Json parts{{"latency_mode", "live-like"},
             {"net_ms", py::round(net, 1)},
             {"net_measured", measured},
             {"hold_ms", s.hold_ms},
             {"server_ms", s.server_ms}};
  if (extra)
    parts["extra_ms"] = extra;
  return {net + s.hold_ms + s.server_ms + extra, parts};
}

std::pair<double, double> Sniper::window() {
  double first = env.trade_window_seconds();
  if (!(first != 0))
    first = INFINITY;
  double last = env.min_seconds_to_expiry() + latency_plan().first / 1000.0;
  return {first, std::max(s.min_left_s, last)};
}

double Sniper::side_held(double end, const std::string &side,
                         const std::string &token) {
  auto rd = rounds.find(end);
  if (rd != rounds.end())
    return rd->second.side(side);
  auto b = round_baseline.find(token);
  return b == round_baseline.end() ? 0.0 : b->second.first;
}

std::pair<double, double> Sniper::kelly_inventory(const std::string &cond,
                                                  double end,
                                                  const std::string &side,
                                                  const std::string &token,
                                                  const std::string &opp) {
  double own = env.open_leg_shares(cond, token);
  double opposite = env.open_leg_shares(cond, opp);
  if (auto live = env.own_risk_shares(token, cond)) {
    own = std::max(own, *live);
  } else {
    own = std::max(own, side_held(end, side, token));
    auto &reserved = inflight[idx(side)];
    if (reserved && reserved->end && *reserved->end == end)
      own += reserved->risk_shares;
  }
  if (!(std::isfinite(own) && own >= 0 && std::isfinite(opposite) &&
        opposite >= 0))
    throw py::ValueError("invalid Kelly inventory");
  return {own, opposite};
}

std::optional<std::pair<double, int>>
Sniper::fair_fee_parameters(const std::string &cond, const std::string &up,
                            const std::string &down) {
  const MarketRules *rules = env.cached_rules(cond, true);
  if (!rules || rules->condition_id != cond)
    return std::nullopt;
  double rate = rules->fee_rate.to_double();
  int exponent = rules->fee_exponent;
  if (!std::isfinite(rate) || !(0 <= rate && rate <= 1) ||
      !(1 <= exponent && exponent <= 8))
    return std::nullopt;
  if (rules->up_token_id && *rules->up_token_id != up)
    return std::nullopt;
  if (rules->down_token_id && *rules->down_token_id != down)
    return std::nullopt;
  return std::pair{rate, exponent};
}

long long Sniper::share_order_floor(const std::string &cond,
                                    const Dec &price_cap) {
  long long override = static_cast<long long>(s.venue_min_shares);
  if (override > 0)
    return hybrid::minimum_entry_shares(price_cap, D(override),
                                        D(MIN_ORDER_USD));
  const MarketRules *rules = env.cached_rules(cond, false);
  Dec minimum = D(static_cast<long long>(KELLY_MIN_SHARES));
  if (rules && rules->min_order_size) {
    Dec v = *rules->min_order_size;
    if (v.zero() || v.neg)
      throw py::ValueError("invalid venue minimum");
    minimum = py::max(minimum, v);
  }
  return hybrid::minimum_entry_shares(price_cap, minimum, D(MIN_ORDER_USD));
}

PxLevels Sniper::visible_levels(const std::string &token,
                                const BookView &view) {
  if (env.own_fill_ttl_s() > 0 && env.has_recent_own_fill(token))
    if (auto net = env.net_asks(view))
      return *net;
  return view.asks;
}

std::optional<double> Sniper::visible_ask(const std::string &token,
                                          const BookView &view) {
  if (!(env.own_fill_ttl_s() > 0))
    return view.best_ask();
  if (!env.has_recent_own_fill(token))
    return view.best_ask();
  auto net = env.net_asks(view);
  if (!net)
    return view.best_ask();
  if (net->empty())
    return std::nullopt;
  return net->front().first;
}

std::pair<std::optional<double>, std::string>
Sniper::fair_anchor(const std::string &token, const BookView &view, Json &row,
                    double t_ms) {
  auto now = view.mid();
  const Json &win = field(row, "window_ms");
  if (!now)
    return {std::nullopt, "no market price"};
  if (win.is_null())
    return {std::nullopt, "history unavailable"};
  std::optional<double> then;
  try {
    auto past =
        env.book().view_at_mono(token, (t_ms - win.get<double>()) / 1000.0);
    if (past)
      then = past->mid();
  } catch (...) {
    then.reset();
  }
  if (!then)
    return {std::nullopt, "history unavailable"};
  row["market_mid_now"] = py::round(*now, 4);
  if (*then <= *now)
    return {then, "before move"};
  return {now, "now"};
}

// ------------------------------------------------------------ cash
void Sniper::set_bankroll(std::optional<double> v, double read_at) {
  double cut = read_at - BANKROLL_SETTLE_S;
  std::set<long long> pending;
  for (auto &[k, sp] : pending_spends_)
    pending.insert(sp.id);
  std::vector<Spend> kept;
  for (auto &sp : spends_)
    if (sp.t >= cut || pending.count(sp.id))
      kept.push_back(sp);
  spends_ = std::move(kept);
  if (!v)
    bankroll_.reset();
  else {
    std::vector<double> amounts;
    for (auto &sp : spends_)
      amounts.push_back(sp.amount);
    bankroll_ = *v - py::builtin_sum(amounts);
  }
  bankroll_at_ = read_at;
}

void Sniper::refresh_bankroll(double read_at) {
  set_bankroll(env.read_bankroll(), read_at);
}

std::optional<double> Sniper::bankroll_now() {
  if (env.live())
    return env.read_bankroll();
  if (!bankroll_ || env.mono() - bankroll_at_ > BANKROLL_STALE_S)
    set_bankroll(env.read_bankroll(), env.mono());
  return bankroll_;
}

void Sniper::bankroll_spent(double amount, Plan *plan) {
  Spend sp{env.mono(), amount, ++spend_ids_};
  spends_.push_back(sp);
  if (plan) {
    ++spend_seq_;
    plan->paper_spend_token = spend_seq_;
    pending_spends_[spend_seq_] = sp;
  }
  if (bankroll_)
    *bankroll_ -= amount;
}

void Sniper::bankroll_completed(Plan &plan) {
  if (!plan.paper_spend_token)
    return;
  auto it = pending_spends_.find(*plan.paper_spend_token);
  if (it == pending_spends_.end())
    return;
  Spend spend = it->second;
  pending_spends_.erase(it);
  Spend done{env.mono(), spend.amount, spend.id};
  for (auto &e : spends_)
    if (e.id == spend.id) {
      e = done;
      return;
    }
  spends_.push_back(done);
}

// ------------------------------------------------------------ hybrid
hybrid::Factors Sniper::hybrid_factors(Json *out) {
  if (!hybrid_risk)
    throw py::ValueError(hybrid_risk_error.empty() ? "hybrid accounting not ready"
                                                   : hybrid_risk_error);
  double age = env.mono() - hybrid_risk->observed_mono;
  if (!std::isfinite(age) || !(0 <= age && age <= HYBRID_RISK_STALE_S))
    throw py::ValueError("hybrid accounting stale");
  double equity = std::max(0.0, hybrid_risk->equity);
  double high_water = hybrid_risk->high_water;
  auto sigma = vol.vol();
  if (!sigma)
    throw py::ValueError("sigma must be a finite number");
  auto f = hybrid::sizing_factors(s.hybrid_policy, *sigma, equity, high_water,
                                  s.kelly_fraction);
  if (out) {
    *out = factors_json(f);
    (*out)["equity"] = equity;
    (*out)["high_water"] = high_water;
    (*out)["accounting_age_s"] = age;
  }
  return f;
}

void Sniper::check_hybrid_order(Plan &plan, const Dec &spend, double fee_rate,
                                const Dec &quoted) {
  if (!plan.hybrid)
    return;
  auto current = hybrid_factors();
  if (current.effective_fraction <= 0)
    throw py::ValueError("hybrid drawdown stop reached");
  if (current.effective_fraction + 1e-12 < plan.hybrid->effective_fraction)
    throw py::ValueError("hybrid allocation tightened before dispatch; fresh "
                         "decision required");
  std::optional<double> budget;
  for (auto v : {plan.risk_budget_usd, current.risk_budget_usd})
    if (v)
      budget = budget ? std::min(*budget, *v) : *v;
  if (budget) {
    hybrid::enforce_order_budget(spend, D(fee_rate), D(*budget), quoted);
    plan.risk_budget_usd = budget;
  }
}

void Sniper::hybrid_drawdown_pause(Json &snapshot) {
  double pause = s.hybrid_dd_pause_s;
  if (pause <= 0)
    return;
  double equity, peak;
  bool stopped;
  try {
    equity = std::max(0.0, snapshot.at("equity").get<double>());
    peak = snapshot.at("high_water").get<double>();
    stopped = hybrid::at_drawdown_stop(s.hybrid_policy, equity, peak);
  } catch (...) {
    return;
  }
  double now = env.mono();
  if (!stopped) {
    dd_stop_since.reset();
    return;
  }
  if (!dd_stop_since) {
    dd_stop_since = now;
    write(Json{{"wall", env.wall()},
               {"result", "hybrid_drawdown_pause"},
               {"pause_s", pause},
               {"equity", py::round(equity, 4)},
               {"high_water", py::round(peak, 4)}});
    return;
  }
  if (now - *dd_stop_since < pause)
    return;
  Json fresh;
  try {
    fresh = env.hybrid_risk_view(std::nullopt, std::nullopt, true);
  } catch (...) {
    ++n["hybrid drawdown reset errors"];
    return;
  }
  dd_stop_since.reset();
  ++n["hybrid drawdown resets"];
  write(Json{{"wall", env.wall()},
             {"result", "hybrid_drawdown_reset"},
             {"old_high_water", py::round(peak, 4)},
             {"equity", py::round(fresh.at("equity").get<double>(), 4)}});
  snapshot = fresh;
}

void Sniper::refresh_hybrid_risk() {
  try {
    std::optional<double> reference;
    std::optional<long long> expected;
    if (!env.hybrid_initialized()) {
      expected = env.journal_seq();
      auto cash = env.read_bankroll();
      if (!cash)
        throw py::ValueError("hybrid reference cash unavailable");
      reference = std::max(0.0, *cash) + env.open_basis();
    }
    double observed = env.mono();
    Json snapshot = env.hybrid_risk_view(reference, expected, false);
    hybrid_drawdown_pause(snapshot);
    HybridSnapshot h;
    h.equity = snapshot.at("equity").get<double>();
    h.high_water = snapshot.at("high_water").get<double>();
    h.observed_mono = observed;
    h.raw = snapshot;
    hybrid_risk = h;
    hybrid_risk_error.clear();
  } catch (const std::exception &e) {
    hybrid_risk_error = error_text(e);
    hybrid_risk.reset();
  }
}

void Sniper::seed_minimum_entries(const std::vector<Json> &rows) {
  try {
    double now = env.wall();
    std::set<std::tuple<std::string, std::string, double>> claims;
    for (auto &row : rows) {
      if (!row.is_object())
        continue;
      if (field(row, "minimum_entry") == Json(true) &&
          field(row, "minimum_entry_attempted") == Json(true)) {
        const Json &key = field(row, "minimum_entry_key");
        if (!key.is_array() || key.size() != 3)
          throw py::ValueError("minimum-entry attempt has an invalid key");
        if (!key[0].is_string() || key[0].get<std::string>().empty() ||
            !key[1].is_string() || key[1].get<std::string>().empty() ||
            !key[2].is_number() || !(key[2].get<double>() > 0))
          throw py::ValueError("minimum-entry attempt has an invalid round");
        double end = key[2].get<double>();
        if (end > now)
          claims.insert({key[0].get<std::string>(), key[1].get<std::string>(),
                         end});
      }
    }
    minimum_entry_keys.insert(claims.begin(), claims.end());
    minimum_entry_seed_error.clear();
  } catch (const std::exception &e) {
    minimum_entry_seed_error = error_text(e);
  }
}

// ------------------------------------------------------------ one order
std::shared_ptr<Plan> Sniper::fire_now(const std::string &side, double r_bp,
                                       double t_ms, std::int64_t t_ns) {
  if (closing) {
    release(side);
    return nullptr;
  }
  double wall = env.wall();
  Json row;
  row["wall"] = wall;
  row["id"] = decision_id(wall, side);
  row["side"] = side;
  row["bp"] = py::round(r_bp, 3);
  const std::string feed = fire_feed_.empty() ? s.primary_feed : fire_feed_;
  row["feed"] = feed;
  row["loop_lag_ms"] = py::round(env.mono() * 1000.0 - t_ms, 1);
  Json window = window_fields(trigger_of(feed), t_ms);
  for (auto &[k, v] : window.items())
    row[k] = v;
  if (t1_mid_)
    row["signal_mid"] = *t1_mid_;
  if (const auto &fresh = freshness(feed); fresh.last_excess)
    row[feed + "_excess_ms"] = py::round(*fresh.last_excess, 1);
  if (retry_fields_)
    for (auto &[k, v] : retry_fields_->items())
      row[k] = v; // keeps the signal's own window and reference
  try {
    auto plan = decide(side, row, t_ms);
    if (!plan) {
      release(side);
      return nullptr;
    }
    plan->t_ms = t_ms;
    plan->feed = feed;
    plan->t_ns = t_ns;
    plan->r_bp = r_bp;
    plan->retry = retry_next_;
    if (env.live()) {
      // _fire_live: the decision-book fields join the row once the order
      // is on its way; a refusal is journalled as a skip, nothing was sent
      Json sent_row = row;
      decision_book(sent_row, *plan);
      plan->row = sent_row;
      plan->sent_wall = env.wall();
      plan->sent_mono = env.mono();
      if (!env.dispatch_live(plan)) {
        auto &orders = n["orders"];
        if (orders > 0)
          --orders; // decided, but not sent
        if (plan->minimum_entry)
          minimum_entry_keys.erase(plan->minimum_entry_key);
        if (!plan->live_refusal.empty()) {
          row["detail"] = plan->live_refusal.substr(0, 200);
          skip(plan->live_halted ? "live halted" : "live refused", row);
        }
        release(side);
        return nullptr;
      }
      last_plan_[idx(side)] = plan;
      return plan;
    }
    std::string sign_err;
    {
      // _sign: create_market_order's own price range check
      double t = py::parse_float(plan->tick);
      Dec td = D(plan->tick);
      double price =
          (py::to_integral(D(plan->cap) / td, py::Rounding::Floor) * td)
              .to_double() +
          1e-9;
      if (!(t <= price && price <= 1 - t + 1e-9))
        sign_err = "order signing failed: invalid price (" +
                   py::repr(plan->cap) + "), min: " + py::repr(t) +
                   " - max: " + py::repr(1 - t);
    }
    if (sign_err.empty() && plan->hybrid) {
      try {
        check_hybrid_order(*plan, D(plan->amount), *plan->fair_fee_rate);
      } catch (const py::ValueError &e) {
        sign_err = std::string("hybrid risk check: ") + e.what();
      }
    }
    plan->sent_wall = env.wall();
    plan->sent_mono = env.mono();
    if (!sign_err.empty()) {
      if (plan->minimum_entry)
        minimum_entry_keys.erase(plan->minimum_entry_key);
      plan->sign_error = sign_err;
    } else {
      if (plan->minimum_entry) {
        row["minimum_entry_attempted"] = true;
        auto &[c, t, e] = plan->minimum_entry_key;
        write(Json{{"wall", env.wall()},
                   {"result", "minimum_entry_attempt"},
                   {"minimum_entry", true},
                   {"minimum_entry_attempted", true},
                   {"minimum_entry_key", Json::array({c, t, e})}});
      }
      double reserve =
          hybrid::risk_total(D(plan->amount),
                             D(plan->fair_fee_rate ? *plan->fair_fee_rate : 0.0))
              .to_double();
      bankroll_spent(reserve, plan.get());
    }
    decision_book(row, *plan);
    plan->row = row;
    dispatched_.push_back(plan);
    last_plan_[idx(side)] = plan;
    return plan;
  } catch (const std::exception &e) {
    ++n["errors"];
    row["result"] = "error";
    row["reason"] = error_text(e);
    write(row);
    release(side);
    return nullptr;
  }
}

std::shared_ptr<Plan> Sniper::decide(const std::string &side, Json &row,
                                     double t_ms) {
  std::string up = env.up_token(), down = env.down_token(),
              cond = env.condition_id();
  auto end_opt = env.window_end();
  if (up.empty() || down.empty() || cond.empty() || !end_opt || *end_opt == 0)
    return skip("no current round", row);
  double end = *end_opt;
  std::string token = side == "UP" ? up : down;
  // (asked first: the resting order itself also awaits its fill size)
  if (env.resting_in(cond))
    return skip("our resting order is in this market", row);
  if (env.awaiting_fak(side, cond, token))
    return skip("awaiting FAK fill size", row);
  double left = end - env.wall();
  row["round_end"] = end_json(end);
  row["left_s"] = py::round(left, 1);
  row["condition_id"] = cond;
  Json ptb = env.ptb_view(side, end);
  if (ptb.empty())
    ptb = ptb_view(side, end);
  for (auto &[k, v] : ptb.items())
    row[k] = v;
  auto [first, last] = window();
  if (left > first)
    return skip("before trade window", row);
  if (left < last)
    return skip("too late in round", row);
  if (s.ptb_skip_close &&
      starts_with(field(row, "ptb_why").is_string()
                      ? row["ptb_why"].get<std::string>()
                      : "",
                  "within "))
    return skip("too close to the price to beat", row);
  BookView view = env.book().view(token);
  if (view.status != LIVE) {
    row["book_status"] = view.status;
    return skip("book not live", row);
  }
  if (view.asks.empty())
    return skip("no asks on this side", row);
  if (view.updated_mono &&
      (env.mono() - *view.updated_mono) * 1000.0 > s.stale_ms)
    return skip("book stale", row);
  auto ask_opt = visible_ask(token, view);
  if (!ask_opt)
    return skip("no asks left after our own recent fills", row);
  double ask = *ask_opt;
  double lo = env.min_buy_price(), hi = env.max_buy_price();
  if (!(hi != 0))
    hi = 1.0;
  if (!(lo <= ask && ask <= hi)) {
    row["ask_seen"] = ask;
    return skip("ask outside price band", row);
  }
  double cap = std::min(0.99, py::round(ask + s.slip, 4));
  std::optional<double> fair, fee_rate;
  std::optional<int> fee_exponent;
  if (s.fair) {
    auto sigma = vol.vol();
    auto [mkt, anchor] = fair_anchor(token, view, row, t_ms);
    double tau = end - env.wall();
    double move = std::abs(field(row, "bp").is_number()
                               ? row["bp"].get<double>()
                               : 0.0);
    std::string why = !sigma                                    ? "volatility warming up"
                      : (!mkt && anchor == "history unavailable") ? "no pre-move market history"
                      : !mkt                                      ? "no market price"
                      : move <= 0                                 ? "no move"
                      : tau <= 0                                  ? "round over"
                                                                  : "";
    if (!why.empty())
      return skip("fair value unknown: " + why, row);
    auto fees = fair_fee_parameters(cond, up, down);
    if (!fees)
      return skip("fair value unknown: market fee rules not loaded", row);
    fee_rate = fees->first;
    fee_exponent = fees->second;
    double f = anchored_fair(*mkt, move, *sigma, tau);
    double tick = view.tick_size && *view.tick_size ? *view.tick_size : 0.01;
    double limit = fair_limit(f, s.fair_margin, tick, *fee_rate, *fee_exponent);
    if (s.fair_no_limit) {
      row["fair_limit_with_fee"] = limit;
      limit = fair_limit(f, s.fair_margin, tick, 0.0, *fee_exponent);
    }
    row["market_mid"] = py::round(*mkt, 4);
    row["fair_anchor"] = anchor;
    row["fair"] = py::round(f, 4);
    row["fair_limit"] = limit;
    row["vol_bp"] = py::round(*sigma * 1e4, 3);
    row["tau_s"] = py::round(tau, 1);
    row["fee_rate"] = *fee_rate;
    row["fee_exponent"] = *fee_exponent;
    row["fair_gap_c"] = py::round((limit - ask) * 100.0, 2);
    if (ask > limit + 1e-9) {
      row["ask_seen"] = ask;
      return skip("ask not below fair value", row);
    }
    double gap_c = s.min_gap_c;
    if (gap_c > 0 && limit + 1e-9 < ask + gap_c / 100.0) {
      row["ask_seen"] = ask;
      return skip("fair limit under ask + SNIPER_MIN_GAP_C", row);
    }
    if (!s.fair_no_limit)
      cap = std::min(cap, limit);
    fair = f;
  }
  if (lo > 0 && cap <= lo + 1e-9) {
    row["ask_seen"] = ask;
    row["cap"] = cap;
    return skip("price cap at the price floor", row);
  }
  std::optional<double> target, risk_budget;
  std::optional<hybrid::Factors> hyb;
  bool minimum_entry = false, kelly = false;
  auto minimum_key = std::make_tuple(cond, token, end);
  std::optional<long long> share_floor;
  double room = 0, bank = 0, amount = 0;
  auto tick_dec = [&]() {
    return D(view.tick_size && *view.tick_size ? *view.tick_size : 0.01);
  };
  auto limit_dec = [&]() {
    Dec t = tick_dec();
    return py::to_integral(D(std::min(cap, hi)) / t, py::Rounding::Floor) * t;
  };
  if (s.order_type == "FAK") {
    if (fair && s.kelly) {
      auto bk = bankroll_now();
      if (!bk)
        return skip("kelly size unknown: no cash reading", row);
      bank = *bk;
      room = s.max_side_shares - side_held(end, side, token);
      if (auto live_cap = env.order_share_cap())
        room = std::min(room, *live_cap);
      if (s.order_sizing == "shares") {
        Dec signed_limit = limit_dec();
        try {
          share_floor = share_order_floor(cond, signed_limit);
          double fit = static_cast<double>(hybrid::shares_within_budget(
              D(std::max(bank, 0.0)), signed_limit, D(*fee_rate)));
          room = std::min(room, fit);
        } catch (const py::ValueError &e) {
          row["detail"] = e.what();
          return skip("venue minimum unavailable", row);
        }
      }
      auto [held, opposite] =
          kelly_inventory(cond, end, side, token, side == "UP" ? down : up);
      double fraction = s.kelly_fraction, drawdown_scale = 1.0, drawdown = 0.0;
      if (s.hybrid_sizing) {
        Json hj;
        try {
          hyb = hybrid_factors(&hj);
        } catch (const py::ValueError &e) {
          row["detail"] = e.what();
          return skip("hybrid risk unavailable", row);
        }
        row["hybrid"] = hj;
        fraction = hyb->effective_fraction;
        drawdown_scale = hyb->drawdown_scale;
        drawdown = hyb->drawdown;
        risk_budget = hyb->risk_budget_usd;
        if (fraction <= 0)
          return skip("hybrid drawdown stop", row);
        if (risk_budget)
          room = std::min(room, static_cast<double>(hybrid::shares_within_budget(
                                    D(*risk_budget), D(cap), D(*fee_rate))));
      }
      auto [shares, f] = kelly_shares(
          *fair, ask, s.fair_no_limit ? 0.0 : *fee_rate, fraction, bank,
          std::max(room, 0.0), held, opposite, *fee_exponent);
      row["kelly_f"] = py::round(f, 4);
      row["bankroll"] = py::round(bank, 2);
      row["kelly_shares"] = shares;
      row["kelly_held_shares"] = py::round(held, 4);
      row["kelly_opposite_shares"] = py::round(opposite, 4);
      long long required = share_floor ? *share_floor : KELLY_MIN_SHARES;
      if (shares < required && s.kelly_min_entry && share_floor) {
        if (!minimum_entry_seed_error.empty()) {
          row["detail"] = minimum_entry_seed_error;
          return skip("minimum entry state unavailable", row);
        }
        if (minimum_entry_keys.count(minimum_key))
          return skip("minimum entry already attempted this side and round",
                      row);
        if (drawdown_scale < 1.0 &&
            drawdown > s.min_entry_max_drawdown + 1e-12) {
          row["minimum_entry_blocked"] = "realized drawdown";
        } else if (held <= 1e-9 && f > 0 && fraction > 0 && bank > 0 &&
                   room + 1e-9 >= static_cast<double>(*share_floor)) {
          shares = static_cast<double>(*share_floor);
          minimum_entry = true;
          row["minimum_entry"] = true;
          row["minimum_entry_attempted"] = false;
          row["minimum_entry_key"] = Json::array({cond, token, end});
          row["minimum_entry_shares"] = *share_floor;
        }
      }
      if (shares < required) {
        row["ask_seen"] = ask;
        return skip("kelly size under the " + std::to_string(required) +
                        "-share minimum",
                    row);
      }
      target = shares;
      kelly = true;
    } else {
      target = s.shares;
    }
    amount = std::max(
        ioc_spend(visible_levels(token, view), *target, ask, std::min(cap, hi)),
        MIN_ORDER_USD);
    if (kelly && s.order_sizing != "shares") {
      Dec max_spend = py::quantize(D(std::max(room, 0.0)) * D(ask), D("0.01"),
                                   py::Rounding::Floor);
      amount = std::min(amount, max_spend.to_double());
      Dec minimum = py::quantize(D(static_cast<long long>(KELLY_MIN_SHARES)) *
                                     limit_dec(),
                                 D("0.01"), py::Rounding::Ceiling);
      if (amount < std::max(MIN_ORDER_USD, minimum.to_double())) {
        row["ask_seen"] = ask;
        row["amount"] = py::round(amount, 4);
        row["room_shares"] = py::round(room, 4);
        return skip("kelly room under venue minimum at price cap", row);
      }
    }
  } else {
    amount = std::max(s.shares * ask, MIN_ORDER_USD);
  }
  double gate_amount = amount;
  std::optional<long long> order_shares;
  if (s.order_type == "FAK" && s.order_sizing == "shares") {
    Dec limit = limit_dec();
    long long floor_shares =
        share_floor ? *share_floor : share_order_floor(cond, limit);
    long long os = hybrid::cent_compatible_shares(D(*target), limit);
    if (kelly && (os < floor_shares || static_cast<double>(os) > room + 1e-9)) {
      row["ask_seen"] = ask;
      row["order_shares"] = os;
      row["room_shares"] = py::round(room, 4);
      return skip("kelly room under venue minimum at price cap", row);
    }
    os = std::max(os, floor_shares);
    Dec signed_spend = D(os) * limit;
    if (signed_spend != py::quantize(signed_spend, D("0.01"))) {
      row["ask_seen"] = ask;
      row["order_shares"] = os;
      return skip("share order is not a whole-cent amount at price cap", row);
    }
    amount = signed_spend.to_double();
    if (kelly) {
      try {
        hybrid::enforce_order_budget(signed_spend, D(*fee_rate),
                                     D(std::max(bank, 0.0)));
      } catch (const py::ValueError &e) {
        row["ask_seen"] = ask;
        row["amount"] = amount;
        row["detail"] = e.what();
        return skip("kelly cash below signed order reservation", row);
      }
    }
    order_shares = os;
  }
  auto rit = rounds.find(end);
  if (rit == rounds.end()) {
    for (auto it = rounds.begin(); it != rounds.end();)
      it = it->first < end - 3600 ? rounds.erase(it) : std::next(it);
    RoundCounts rd;
    for (auto &[s_, tok] :
         {std::pair<std::string, std::string>{"UP", up}, {"DOWN", down}}) {
      auto b = round_baseline.find(tok);
      if (b != round_baseline.end()) {
        rd.side(s_) += b->second.first;
        rd.fills += b->second.second;
      }
    }
    rit = rounds.emplace(end, rd).first;
  }
  RoundCounts &rd = rit->second;
  auto &other = inflight[1 - idx(side)];
  int in_flight = (other && other->end && *other->end == end) ? 1 : 0;
  if (order_shares) {
    double bound;
    if (kelly)
      bound = room;
    else {
      bound = s.max_side_shares - rd.side(side);
      if (auto live_cap = env.order_share_cap())
        bound = std::min(bound, *live_cap);
    }
    if (static_cast<double>(*order_shares) > std::max(bound, 0.0) + 1e-9)
      return skip("round cap", row);
  }
  double gate_shares =
      order_shares ? static_cast<double>(*order_shares) : gate_amount / ask;
  if (rd.fills + in_flight >= s.max_fills_round ||
      rd.side(side) + gate_shares > s.max_side_shares)
    return skip("round cap", row);
  if (risk_budget) {
    try {
      hybrid::enforce_order_budget(D(amount), D(*fee_rate), D(*risk_budget));
    } catch (const py::ValueError &e) {
      row["detail"] = e.what();
      return skip("hybrid order risk cap", row);
    }
  }
  double risk_shares =
      order_shares ? static_cast<double>(*order_shares) : amount / ask;
  inflight[idx(side)] = InFlight{end, amount, risk_shares};
  if (minimum_entry) {
    double now = env.wall();
    for (auto it = minimum_entry_keys.begin(); it != minimum_entry_keys.end();)
      it = std::get<2>(*it) > now ? std::next(it) : minimum_entry_keys.erase(it);
    minimum_entry_keys.insert(minimum_key);
  }
  row["ask_seen"] = ask;
  row["cap"] = cap;
  row["shares"] = order_shares ? Json(*order_shares)
                               : Json(py::round(risk_shares, 4));
  row["amount"] = py::round(amount, 4);
  if (order_shares) {
    row["order_shares"] = *order_shares;
    row["book_cost"] = py::round(gate_amount, 4);
  }
  if (target)
    row["target_shares"] = py::round(*target, 4);
  ++n["orders"];
  auto [lat, parts] = latency_plan();
  row["latency_ms"] = py::round(lat, 1);
  for (auto &[k, v] : parts.items())
    row[k] = v;
  auto plan = std::make_shared<Plan>();
  plan->side = side;
  plan->end = end;
  plan->token = token;
  plan->other = side == "UP" ? down : up;
  plan->cond = cond;
  plan->up = up;
  plan->down = down;
  plan->amount = amount;
  plan->cap = cap;
  plan->lat = lat;
  plan->view = view;
  plan->dispatch_deadline = end - last;
  plan->risk_shares = risk_shares;
  plan->fair_fee_rate = fee_rate;
  plan->fair_fee_exponent = fee_exponent;
  plan->shares = order_shares;
  plan->hybrid = hyb;
  plan->risk_budget_usd = risk_budget;
  if (order_shares && static_cast<long long>(s.venue_min_shares) > 0)
    plan->venue_min_shares = static_cast<long long>(s.venue_min_shares);
  plan->minimum_entry = minimum_entry;
  plan->minimum_entry_key = minimum_key;
  plan->target = target;
  plan->order_type = s.order_type;
  plan->tick = view.tick_size && *view.tick_size ? py::fmt_g(*view.tick_size)
                                                 : "0.01";
  plan->t0 = t_ms / 1000.0;
  plan->decided_mono = env.mono();
  return plan;
}

void Sniper::finish_paper(const std::shared_ptr<Plan> &plan,
                          const PaperResult &res) {
  Json &row = plan->row;
  std::string outcome;
  try {
    bankroll_completed(*plan);
    if (!res.arrival.is_null())
      row["arrival"] = res.arrival;
    bool filled = res.filled && plan->sign_error.empty();
    std::string reason = plan->sign_error.empty() ? res.error : plan->sign_error;
    row["outcome"] = paper_outcome(filled, filled ? "" : reason, row,
                                   s.book_clock != "wake");
    outcome = row["outcome"].get<std::string>();
    if (!res.arrival.is_null()) {
      const Json &a = res.arrival;
      if (field(a, "evidence") == Json("quiet")) {
        row["book_evidence"] = "quiet";
        ++n["paper orders on an inferred quiet-feed book"];
      }
      row["arrival_ms"] = field(a, "arrival_ms");
    }
    RoundCounts &rd = rounds[plan->end];
    if (filled) {
      ++n["filled"];
      double shares = res.fill.value("shares", 0.0);
      rd.fills += 1;
      rd.side(plan->side) += shares;
      if (plan->target && shares > *plan->target + 1e-6) {
        row["over_target"] = py::round(shares - *plan->target, 4);
        ++n["fills above target (ask fell)"];
      }
      row["result"] = "filled";
      row["order_id"] = field(res.fill, "order_id");
      row["shares"] = field(res.fill, "shares");
      row["price"] = field(res.fill, "average_price");
      row["fee"] = field(res.fill, "fee");
      row["cost"] = field(res.fill, "total_cost");
      if (field(res.fill, "partial") == Json(true)) {
        row["partial"] = true;
        row["stake"] = field(res.fill, "stake");
      }
      if (field(res.fill, "matched_wall").is_number() &&
          field(row, "loop_lag_ms").is_number())
        row["match_ms"] = py::round(
            (res.fill["matched_wall"].get<double>() - row["wall"].get<double>()) *
                    1000.0 +
                row["loop_lag_ms"].get<double>(),
            1);
      write(row);
    } else {
      ++n["refused"];
      row["result"] = "refused";
      row["reason"] = reason;
      write(row);
    }
  } catch (const std::exception &e) {
    ++n["errors"];
    row["result"] = "error";
    row["reason"] = error_text(e);
    write(row);
  }
  release(plan->side);
  if (retryable_outcome(outcome))
    retry_after_miss(plan->side, plan.get());
}

bool Sniper::retryable_outcome(const std::string &outcome) {
  return outcome == "PRICE_CAP_MISS" || outcome == "LIVE_ODDS_MISS" ||
         outcome == "INSUFFICIENT_LIQUIDITY" || outcome == "MISS_AT_DECISION" ||
         outcome == "FOK_MISS_AT_WAKE";
}

void Sniper::retry_after_miss(const std::string &side, const Plan *missed) {
  if (s.miss_retry <= 0 || closing)
    return;
  const int i = idx(side);
  auto prev = last_plan_[i];
  if (!prev || (missed && missed != prev.get()))
    return;
  last_plan_[i].reset();
  if (prev->retry >= s.miss_retry) {
    ++n["miss retry: already re-tried"];
    return;
  }
  const double now_ms = env.mono() * 1000.0;
  if (!(now_ms - prev->t_ms <= s.miss_retry_ms)) {
    ++n["miss retry: signal too old"];
    return;
  }
  if (inflight[i]) {
    ++n["miss retry: order in flight"];
    return;
  }
  // The move must still be there: the latest Coinbase mid against the
  // signal's own reference, the same way and at least the trigger size.
  // On the feed that signalled: its reference is at that feed's own level.
  const Json &ref = field(prev->row, "ref_mid");
  const auto feed_mid = mid_of(prev->feed);
  if (!ref.is_number() || !(ref.get<double>() > 0) || !feed_mid) {
    ++n["miss retry: move unknown"];
    return;
  }
  const double r_now = (*feed_mid / ref.get<double>() - 1.0) * 1e4;
  if ((side == "UP" ? r_now : -r_now) < s.k_bp - 1e-9) {
    ++n["miss retry: move faded"];
    return;
  }
  Json fields{{"retry_of", field(prev->row, "id")},
              {"retry", prev->retry + 1},
              {"retry_after_ms", py::round(now_ms - prev->t_ms, 1)},
              {"ref_mid", ref}};
  if (field(prev->row, "window_ms").is_number())
    fields["window_ms"] = field(prev->row, "window_ms");
  inflight[i] = InFlight{};
  retry_fields_ = std::move(fields);
  retry_next_ = prev->retry + 1;
  t1_mid_ = feed_mid;
  fire_feed_ = prev->feed;
  // The signal's own time: the fair value keeps its pre-move anchor, and the
  // live send guard still refuses it once the trigger has gone stale.
  auto plan = fire_now(side, r_now, prev->t_ms, prev->t_ns);
  fire_feed_ = s.primary_feed;
  t1_mid_.reset();
  retry_fields_.reset();
  retry_next_ = 0;
  if (plan) {
    last_fire_by_side[i] = now_ms;
    ++n["miss retries"];
  }
}
} // namespace sniper

namespace sniper {
// ------------------------------------------------------------ price to beat
double PtbCalibration::basis() const {
  if (diffs.empty())
    return 0.0;
  std::vector<double> v(diffs.begin(), diffs.end());
  std::sort(v.begin(), v.end());
  std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
double PtbCalibration::margin_bp(double floor_bp) const {
  double m = floor_bp;
  for (double e : errors)
    m = std::max(m, e);
  return m;
}
Json PtbCalibration::add(double start, std::optional<double> raw,
                         std::optional<double> est, int basis_rounds,
                         double official) {
  if (seen.count(start))
    return Json::object();
  seen.insert(start);
  std::optional<double> err;
  if (est && basis_rounds >= 3) {
    err = std::abs(official - *est) / official * 1e4;
    errors.push_back(*err);
    if (errors.size() > 24)
      errors.pop_front();
  }
  if (raw) {
    diffs.push_back(official - *raw);
    if (diffs.size() > 12)
      diffs.pop_front();
  }
  return Json{{"official", official},
              {"raw", raw ? Json(*raw) : Json()},
              {"est", est ? Json(*est) : Json()},
              {"basis_rounds", basis_rounds},
              {"err_bp", err ? Json(py::round(*err, 3)) : Json()},
              {"basis_now", py::round(basis(), 4)},
              {"checks", checks()}};
}

namespace {
std::pair<std::optional<double>, std::string>
twap(const std::deque<std::pair<double, double>> &samples, double start,
     double max_gap, double span = 60.0) {
  double begin = start - span;
  std::optional<std::pair<double, double>> prior;
  std::vector<std::pair<double, double>> inside;
  for (auto &[t, p] : samples) {
    if (t <= begin)
      prior = std::pair{t, p};
    else if (t <= start)
      inside.emplace_back(t, p);
  }
  if (!prior)
    return {std::nullopt, "no trigger price " + py::fmt_g(span) +
                              "s before the round's start (started or "
                              "reconnected too late)"};
  std::vector<double> times{prior->first};
  for (auto &[t, p] : inside)
    times.push_back(t);
  times.push_back(start);
  double gap = -INFINITY;
  for (std::size_t i = 1; i < times.size(); ++i)
    gap = std::max(gap, times[i] - times[i - 1]);
  if (gap > max_gap + 1e-9)
    return {std::nullopt, "a " + py::fixed(gap, 1) +
                              "s gap in the trigger feed around the " +
                              py::fmt_g(span) + "s window"};
  std::vector<std::pair<double, double>> points{{begin, prior->second}};
  points.insert(points.end(), inside.begin(), inside.end());
  std::vector<double> parts;
  for (std::size_t i = 0; i < points.size(); ++i) {
    double t1 = i + 1 < points.size() ? points[i + 1].first : start;
    parts.push_back(points[i].second * (t1 - points[i].first));
  }
  return {py::builtin_sum(parts) / span, ""};
}
} // namespace

void Sniper::note_primary_mid(double mid, double wall, double mono) {
  primary_mid = std::pair{mid, mono};
  double start = wall - std::fmod(wall, ROUND_S);
  if (!ptb_start_ || start != *ptb_start_) {
    ptb_start_ = start;
    auto [raw, why] = twap(ptb_hist_, start, s.ptb_max_gap_s);
    PtbRound rec;
    rec.raw = raw;
    rec.est = raw ? std::optional(*raw + ptb_calib.basis()) : std::nullopt;
    rec.basis_rounds = static_cast<int>(ptb_calib.diffs.size());
    rec.why = why;
    ptb_rounds_[start] = rec;
    ptb_pending_[start] = rec;
    while (ptb_rounds_.size() > 4)
      ptb_rounds_.erase(ptb_rounds_.begin());
  }
  ptb_hist_.emplace_back(wall, mid);
  double cut = wall - 60.0 - s.ptb_max_gap_s;
  while (ptb_hist_.size() > 1 && ptb_hist_[1].first <= cut)
    ptb_hist_.pop_front();
}

Json Sniper::ptb_view(const std::string &side, double end) {
  auto it = end ? ptb_rounds_.find(end - ROUND_S) : ptb_rounds_.end();
  if (it == ptb_rounds_.end() || !it->second.est) {
    std::string why = it != ptb_rounds_.end() && !it->second.why.empty()
                          ? it->second.why
                          : "this process did not see the round start";
    return Json{{"ptb", nullptr}, {"ptb_why", why}};
  }
  double ptb = *it->second.est;
  Json fields{{"ptb", py::round(ptb, 2)}};
  if (!primary_mid || env.mono() - primary_mid->second > 2.0) {
    fields["ptb_dist_bp"] = nullptr;
    fields["ptb_why"] = "no fresh trigger price";
    return fields;
  }
  double dist = (primary_mid->first - ptb) / ptb * 1e4 *
                (side == "UP" ? 1.0 : -1.0);
  double margin = ptb_calib.margin_bp(s.ptb_margin_bp);
  int checks = ptb_calib.checks();
  fields["ptb_dist_bp"] = py::round(dist, 3);
  fields["ptb_margin_bp"] = py::round(margin, 3);
  fields["ptb_checks"] = checks;
  if (dist <= 0)
    return fields;
  if (checks < s.ptb_min_checks) {
    fields["ptb_why"] = "estimate checked against Polymarket's published price "
                        "to beat on " +
                        std::to_string(checks) + " of " +
                        py::fmt_g(s.ptb_min_checks) + " rounds";
    return fields;
  }
  if (dist <= margin)
    fields["ptb_why"] =
        "within the estimate's error (+-" + py::fixed(margin, 2) + "bp)";
  return fields;
}

void Sniper::ptb_official(double start, double official) {
  auto it = ptb_pending_.find(start);
  if (it == ptb_pending_.end())
    return;
  PtbRound rec = it->second;
  ptb_pending_.erase(it);
  Json fields = ptb_calib.add(start, rec.raw, rec.est, rec.basis_rounds, official);
  if (fields.empty())
    return;
  Json row{{"wall", env.wall()}, {"result", "ptb_official"},
           {"round_start", start}};
  for (auto &[k, v] : fields.items())
    row[k] = v;
  write(row);
}

void Sniper::decision_book(Json &row, const Plan &plan) {
  try {
    const BookView &view = plan.view;
    std::optional<double> bid = view.best_bid(), ask = view.best_ask();
    double hi = env.max_buy_price();
    if (!(hi != 0))
      hi = 1.0;
    double cap = std::min(plan.cap, hi);
    auto levels = visible_levels(plan.token, view);
    std::vector<double> qs, usd;
    long long under = 0;
    for (auto &[px, q] : levels)
      if (px <= cap + 1e-9) {
        ++under;
        qs.push_back(q);
        usd.push_back(px * q);
      }
    row["dec_bid"] = bid ? Json(*bid) : Json();
    row["dec_ask"] = ask ? Json(*ask) : Json();
    row["dec_spread"] =
        bid && ask ? Json(py::round(*ask - *bid, 4)) : Json(nullptr);
    row["dec_cap"] = py::round(cap, 4);
    row["dec_levels_under_cap"] = under;
    row["dec_depth_sh"] = py::round(py::builtin_sum(qs), 4);
    row["dec_depth_usd"] = py::round(py::builtin_sum(usd), 4);
    if (view.updated_mono)
      row["dec_quote_age_ms"] =
          py::round((plan.decided_mono - *view.updated_mono) * 1000.0, 1);
    if (view.exchange_ts_ms)
      row["dec_venue_age_ms"] = py::round(
          row.value("wall", env.wall()) * 1000.0 + s.venue_offset_ms -
              static_cast<double>(*view.exchange_ts_ms),
          1);
  } catch (const std::exception &e) {
    row["dec_error"] = error_text(e).substr(0, 120);
  }
}
} // namespace sniper
