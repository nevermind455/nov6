// Port of sniper_mode.Settings (nevermind455/oct10@772f2eb): the same names,
// defaults, parsing (_env_float/_env_bool) and the same refusal messages.
// Settings this build does not implement are refused when enabled; a
// reference default this build cannot honour is named in `notices`.
#include "sniper/pyrt.hpp"
#include "sniper/sniper.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace sniper {
namespace {
std::string strip(const std::string &s) {
  auto a = s.find_first_not_of(" \t\r\n\f\v"),
       b = s.find_last_not_of(" \t\r\n\f\v");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}
std::string pyrepr(const std::string &s) {
  bool sq = s.find('\'') != std::string::npos,
       dq = s.find('"') != std::string::npos;
  char q = (sq && !dq) ? '"' : '\'';
  std::string out(1, q);
  for (char ch : s) {
    if (ch == q || ch == '\\')
      out += '\\';
    out += ch;
  }
  return out + q;
}
std::string g(double v) { return py::fmt_g(v); }
} // namespace

SniperSettings
sniper_settings(const std::function<std::optional<std::string>(const std::string &)> &get,
                std::vector<std::string> *notices) {
  auto raw_of = [&](const std::string &name) -> std::optional<std::string> {
    return get(name);
  };
  auto env_float = [&](const std::string &name, double def) {
    auto raw = raw_of(name);
    if (!raw || strip(*raw).empty())
      return def;
    double v;
    try {
      v = py::parse_float(strip(*raw));
    } catch (const py::ValueError &) {
      throw std::invalid_argument("invalid " + name + "=" + pyrepr(*raw) +
                                  ": not a number");
    }
    if (!std::isfinite(v))
      throw std::invalid_argument("invalid " + name + "=" + pyrepr(*raw) +
                                  ": must be finite");
    return v;
  };
  auto env_bool = [&](const std::string &name, bool def) {
    auto raw = lower(strip(raw_of(name).value_or("")));
    if (raw.empty())
      return def;
    if (raw == "1" || raw == "true" || raw == "yes" || raw == "on")
      return true;
    if (raw == "0" || raw == "false" || raw == "no" || raw == "off")
      return false;
    throw std::invalid_argument("invalid " + name + "=" + pyrepr(raw) +
                                ": 1 or 0");
  };
  auto env_int = [&](const std::string &name, int def) {
    double v = std::trunc(env_float(name, def));
    if (v < std::numeric_limits<int>::min() ||
        v > std::numeric_limits<int>::max())
      throw std::invalid_argument("invalid " + name + "=" + g(v) +
                                  ": outside native integer range");
    return static_cast<int>(v);
  };
  auto text = [&](const std::string &name, const std::string &def) {
    auto raw = raw_of(name);
    return raw ? *raw : def;
  };
  auto note = [&](const std::string &n) {
    if (notices)
      notices->push_back(n);
  };
  auto refuse_on = [&](const std::string &name, bool on) {
    if (on)
      throw std::invalid_argument(
          name + " is a reference feature this native build does not "
                 "implement; leave it unset or off");
  };
  SniperSettings s;
  s.k_bp = env_float("SNIPER_K_BP", 0.5);
  s.window_ms = env_float("SNIPER_WINDOW_MS", 250);
  s.shares = env_float("SNIPER_SHARES", 10);
  s.slip = env_float("SNIPER_SLIP", 0.05);
  s.fixed_latency = !strip(text("SNIPER_LATENCY_MS", "")).empty();
  s.latency_ms = env_float("SNIPER_LATENCY_MS", 200);
  s.hold_ms = env_float("SNIPER_HOLD_MS", 250);
  s.server_ms = env_float("SNIPER_SERVER_MS", 25);
  s.net_default_ms = env_float("SNIPER_NET_MS", 14);
  std::string delay = lower(strip(text("SNIPER_DELAY_SOURCE", "auto")));
  if (delay.empty())
    delay = "auto";
  if (delay != "auto" && delay != "model" && delay != "live")
    throw std::invalid_argument("invalid SNIPER_DELAY_SOURCE " + pyrepr(delay) +
                                ": auto, model or live");
  double delay_days = env_float("SNIPER_DELAY_DAYS", 3);
  if (delay == "live" && s.fixed_latency)
    throw std::invalid_argument("SNIPER_DELAY_SOURCE=live and SNIPER_LATENCY_MS "
                                "are exclusive: unset SNIPER_LATENCY_MS");
  s.delay_source = delay;
  s.delay_days = delay_days;
  // Default paper requires live controls in the reference (strict paper/live
  // comparison); this build runs the independent paper model only.
  {
    auto raw = raw_of("SNIPER_PAPER_LIVE_PARITY");
    if (!raw || strip(*raw).empty())
      note("SNIPER_PAPER_LIVE_PARITY unset: the reference defaults to 1 "
           "(strict paper/live comparison); this build runs the independent "
           "paper model, as SNIPER_PAPER_LIVE_PARITY=0");
    else
      refuse_on("SNIPER_PAPER_LIVE_PARITY",
                env_bool("SNIPER_PAPER_LIVE_PARITY", true));
  }
  s.paper_extra_ms = env_float("SNIPER_PAPER_EXTRA_MS", 200.0);
  if (!(0.0 <= s.paper_extra_ms && s.paper_extra_ms <= 5000.0))
    throw std::invalid_argument("invalid SNIPER_PAPER_EXTRA_MS " +
                                py::repr(s.paper_extra_ms) + ": 0..5000 ms");
  std::string odds = lower(strip(text("SNIPER_FILL_ODDS", "auto")));
  if (odds.empty())
    odds = "auto";
  if (odds != "auto" && odds != "live" && odds != "off")
    throw std::invalid_argument("invalid SNIPER_FILL_ODDS " + pyrepr(odds) +
                                ": auto, live or off");
  refuse_on("SNIPER_FILL_ODDS=live", odds == "live");
  if (odds == "auto")
    note("SNIPER_FILL_ODDS=auto: measured live fill odds are not implemented; "
         "every book fill is kept (as SNIPER_FILL_ODDS=off)");
  double fill_odds_orders = env_float("SNIPER_FILL_ODDS_ORDERS", 1000);
  s.cooldown_ms = env_float("SNIPER_COOLDOWN_MS", 300);
  s.min_left_s = env_float("SNIPER_MIN_LEFT_S", 10);
  refuse_on("SNIPER_FRESH_MARKET_SOCKET",
            env_bool("SNIPER_FRESH_MARKET_SOCKET", false));
  s.max_fills_round = env_int("SNIPER_MAX_FILLS_ROUND", 20);
  s.max_side_shares = env_float("SNIPER_MAX_SIDE_SHARES", 200);
  s.fair = env_bool("SNIPER_FAIR", true);
  s.fair_margin = env_float("SNIPER_FAIR_MARGIN", 0.01);
  s.min_gap_c = env_float("SNIPER_MIN_GAP_C", 0);
  double wide_cap_c = env_float("SNIPER_WIDE_CAP_C", 0);
  bool wide_cap_live = env_bool("SNIPER_WIDE_CAP_LIVE", false);
  double wide_stop = env_float("SNIPER_WIDE_CAP_LIVE_STOP_USD", 25);
  bool rest_live = env_bool("SNIPER_REST_LIVE", false);
  double rest_s = env_float("SNIPER_REST_S", 5);
  double rest_stop = env_float("SNIPER_REST_LIVE_STOP_USD", 10);
  s.rest_live = rest_live;
  s.rest_s = rest_s;
  s.rest_stop_usd = rest_stop;
  s.rest_dir = env_bool("SNIPER_REST_DIR", true);
  bool maker_only = env_bool("SNIPER_MAKER_ONLY", false);
  s.fair_no_limit = env_bool("SNIPER_FAIR_NO_LIMIT", false);
  double maker_under_c = env_float("SNIPER_MAKER_UNDER_C", 2);
  double maker_split = env_float("SNIPER_MAKER_SPLIT", 0);
  double dir_add = env_float("SNIPER_DIR_ADD", 0);
  bool dir_add_live = env_bool("SNIPER_DIR_ADD_LIVE", false);
  double ptb_max_gap_s = env_float("SNIPER_PTB_MAX_GAP_S", 5);
  s.ptb_max_gap_s = ptb_max_gap_s;
  s.ptb_margin_bp = env_float("SNIPER_PTB_MARGIN_BP", 1.0);
  double ptb_min_checks = env_float("SNIPER_PTB_MIN_CHECKS", 6);
  s.ptb_min_checks = ptb_min_checks;
  s.ptb_skip_close = env_bool("SNIPER_PTB_SKIP_CLOSE", false);
  s.venue_min_shares = env_float("SNIPER_VENUE_MIN_SHARES", 0);
  s.miss_retry = env_int("SNIPER_MISS_RETRY", 0);
  s.miss_retry_ms = env_float("SNIPER_MISS_RETRY_MS", 1000);
  {
    std::string raw = strip(text("SNIPER_PAPER_CASH_FROM", ""));
    std::string l = lower(raw);
    if (l.empty() || l == "auto")
      note("SNIPER_PAPER_CASH_FROM=auto: sizing from a live bot's cash is not "
           "implemented; paper sizes from its own cash (as off)");
    else if (!(l == "off" || l == "0" || l == "no" || l == "none"))
      refuse_on("SNIPER_PAPER_CASH_FROM=<path>", true);
  }
  double paper_cash_max_age_s = env_float("SNIPER_PAPER_CASH_MAX_AGE_S", 900);
  s.fair_vol_s = env_float("SNIPER_FAIR_VOL_S", 300);
  double fair_fee_rate = env_float("SNIPER_FAIR_FEE_RATE", 0.07);
  s.kelly = env_bool("SNIPER_KELLY", true);
  s.flat_live = env_bool("SNIPER_FLAT_LIVE", false);
  s.kelly_fraction = env_float("SNIPER_KELLY_FRACTION", 0.25);
  s.hybrid_sizing = env_bool("SNIPER_HYBRID_SIZING", true);
  s.kelly_min_entry = env_bool("SNIPER_KELLY_MIN_ENTRY", false);
  s.min_entry_max_drawdown = env_float("SNIPER_MIN_ENTRY_MAX_DRAWDOWN", 0.0);
  try {
    s.hybrid_policy.vol_baseline_bp =
        env_float("SNIPER_HYBRID_VOL_BASELINE_BP", 1.0);
    s.hybrid_policy.drawdown_stop = env_float("SNIPER_HYBRID_DRAWDOWN_STOP", 0.20);
    s.hybrid_policy.risk_fraction = env_float("SNIPER_HYBRID_RISK_FRACTION", 0.0);
    s.hybrid_policy.max_order_usd = env_float("SNIPER_HYBRID_MAX_ORDER_USD", 0.0);
    s.hybrid_policy.validate();
  } catch (const py::ValueError &e) {
    throw std::invalid_argument(std::string("invalid SNIPER_HYBRID_* settings: ") +
                                e.what());
  }
  s.hybrid_dd_pause_s = env_float("SNIPER_HYBRID_DRAWDOWN_PAUSE_S", 600.0);
  if (!(0.0 <= s.hybrid_dd_pause_s && s.hybrid_dd_pause_s <= 86400.0))
    throw std::invalid_argument("invalid SNIPER_HYBRID_DRAWDOWN_PAUSE_S " +
                                py::repr(s.hybrid_dd_pause_s) + ": 0..86400 s");
  s.stale_ms = env_float("SNIPER_STALE_MS", 2000);
  s.book_clock = lower(strip(text("SNIPER_BOOK_CLOCK", "exchange")));
  if (s.book_clock != "exchange" && s.book_clock != "local" &&
      s.book_clock != "wake")
    s.book_clock = "exchange";
  refuse_on("SNIPER_BOOK_CLOCK=wake", s.book_clock == "wake");
  s.book_wait_ms = env_float("SNIPER_BOOK_WAIT_MS", 500);
  {
    std::string q = lower(strip(text("SNIPER_BOOK_QUIET", "0")));
    s.book_quiet = q == "1" || q == "true" || q == "yes" || q == "on";
  }
  s.venue_offset_ms = env_float("SNIPER_VENUE_CLOCK_OFFSET_MS", 0);
  std::string order_type = strip(text("SNIPER_ORDER_TYPE", "FAK"));
  std::transform(order_type.begin(), order_type.end(), order_type.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  if (order_type.empty())
    order_type = "FAK";
  if (order_type != "FOK" && order_type != "FAK")
    throw std::invalid_argument("invalid SNIPER_ORDER_TYPE " +
                                pyrepr(order_type) + ": FOK or FAK");
  s.order_type = order_type;
  std::string sizing = lower(strip(text("SNIPER_ORDER_SIZING", "shares")));
  if (sizing.empty())
    sizing = "shares";
  if (sizing != "shares" && sizing != "dollars")
    throw std::invalid_argument("invalid SNIPER_ORDER_SIZING " + pyrepr(sizing) +
                                ": shares or dollars");
  s.order_sizing = sizing;
  s.backlog_gate = lower(strip(text("SNIPER_BACKLOG_GATE", "count")));
  if (s.backlog_gate.empty())
    s.backlog_gate = "count";
  if (s.backlog_gate != "count" && s.backlog_gate != "age")
    throw std::invalid_argument("invalid SNIPER_BACKLOG_GATE " +
                                pyrepr(s.backlog_gate) + ": count or age");
  s.backlog_fresh_ms = env_float("SNIPER_BACKLOG_FRESH_MS", 100);
  {
    std::string c = lower(strip(text("SNIPER_FEED_COALESCE", "0")));
    refuse_on("SNIPER_FEED_COALESCE",
              c == "1" || c == "true" || c == "yes" || c == "on");
  }
  s.primary_feed = lower(strip(text("SNIPER_PRIMARY_FEED", "coinbase")));
  if (s.primary_feed.empty())
    s.primary_feed = "coinbase";
  if (s.primary_feed != "coinbase" && s.primary_feed != "binance" &&
      s.primary_feed != "kraken")
    throw std::invalid_argument("invalid SNIPER_PRIMARY_FEED " +
                                pyrepr(s.primary_feed) +
                                ": coinbase or binance or kraken");
  refuse_on("SNIPER_PRIMARY_FEED=" + s.primary_feed, s.primary_feed != "coinbase");
  {
    std::string f = strip(text("SNIPER_COINBASE_FRESHNESS", "1"));
    s.coinbase_freshness = !(f == "0" || f == "false" || f == "off");
  }
  s.coinbase_stale_ms = env_float("SNIPER_COINBASE_STALE_MS", 1000);
  s.coinbase_unknown_ms = env_float("SNIPER_COINBASE_UNKNOWN_MS", 5000);
  s.coinbase_backlog_max = env_int("SNIPER_COINBASE_BACKLOG_MAX", 20);
  double cb_silent = env_float("SNIPER_COINBASE_SILENT_S", 5);
  s.coinbase_silent_s = cb_silent;
  s.coinbase_conns = env_int("SNIPER_COINBASE_CONNS", 2);
  refuse_on("SNIPER_EXTRA_FEEDS", !strip(text("SNIPER_EXTRA_FEEDS", "")).empty());
  refuse_on("SNIPER_WIDE_CAP_C", wide_cap_c > 0);
  refuse_on("SNIPER_WIDE_CAP_LIVE", wide_cap_live);
  refuse_on("SNIPER_MAKER_ONLY", maker_only);
  refuse_on("SNIPER_MAKER_SPLIT", maker_split > 0);
  refuse_on("SNIPER_DIR_ADD", dir_add > 0);
  refuse_on("SNIPER_DIR_ADD_LIVE", dir_add_live);
  // The reference's combined validation, same order and texts.
  std::vector<std::pair<bool, std::string>> checks = {
      {s.k_bp > 0, "SNIPER_K_BP=" + g(s.k_bp) + " (must be > 0)"},
      {s.window_ms > 0, "SNIPER_WINDOW_MS=" + g(s.window_ms) + " (must be > 0)"},
      {s.shares >= 5, "SNIPER_SHARES=" + g(s.shares) + " (venue minimum is 5)"},
      {0 <= s.slip && s.slip < 0.5,
       "SNIPER_SLIP=" + g(s.slip) +
           " (must be 0 to 0.49; the price limit is also capped at "
           "MAX_BUY_PRICE, so 0.45 already means 'up to MAX_BUY_PRICE' for "
           "asks from 0.50)"},
      {0 <= s.latency_ms && s.latency_ms < 60000,
       "SNIPER_LATENCY_MS=" + g(s.latency_ms) + " (0 to 60000)"},
      {s.max_fills_round > 0, "SNIPER_MAX_FILLS_ROUND=" +
                                  std::to_string(s.max_fills_round) +
                                  " (must be > 0)"},
      {s.cooldown_ms >= 0,
       "SNIPER_COOLDOWN_MS=" + g(s.cooldown_ms) + " (must be >= 0)"},
      {s.min_left_s >= 0,
       "SNIPER_MIN_LEFT_S=" + g(s.min_left_s) + " (must be >= 0)"},
      {s.max_side_shares > 0,
       "SNIPER_MAX_SIDE_SHARES=" + g(s.max_side_shares) + " (must be > 0)"},
      {s.stale_ms > 0, "SNIPER_STALE_MS=" + g(s.stale_ms) + " (must be > 0)"},
      {s.book_wait_ms >= 0,
       "SNIPER_BOOK_WAIT_MS=" + g(s.book_wait_ms) + " (must be >= 0)"},
      {std::min({s.hold_ms, s.server_ms, s.net_default_ms}) >= 0,
       "SNIPER_HOLD_MS / SNIPER_SERVER_MS / SNIPER_NET_MS must be >= 0"},
      {s.coinbase_stale_ms > 0 && s.coinbase_unknown_ms > 0 &&
           s.coinbase_backlog_max >= 0 && cb_silent > 0,
       "SNIPER_COINBASE_STALE_MS / _UNKNOWN_MS / _SILENT_S must be > 0, "
       "_BACKLOG_MAX >= 0"},
      {1 <= s.coinbase_conns && s.coinbase_conns <= 4,
       "SNIPER_COINBASE_CONNS=" + std::to_string(s.coinbase_conns) +
           " (1 to 4)"},
      {0 <= s.fair_margin && s.fair_margin < 0.5,
       "SNIPER_FAIR_MARGIN=" + g(s.fair_margin) + " (must be 0 to 0.49)"},
      {0 <= s.min_gap_c && s.min_gap_c <= 20,
       "SNIPER_MIN_GAP_C=" + g(s.min_gap_c) + " (cents, must be 0 to 20)"},
      {s.min_gap_c == 0 || s.fair,
       "SNIPER_MIN_GAP_C needs SNIPER_FAIR=1 (it is a gap to the fair limit)"},
      {0 <= wide_cap_c && wide_cap_c <= 50,
       "SNIPER_WIDE_CAP_C=" + g(wide_cap_c) + " (cents, must be 0 to 50)"},
      {wide_stop > 0, "SNIPER_WIDE_CAP_LIVE_STOP_USD=" + g(wide_stop) +
                          " (dollars, must be > 0)"},
      {1 <= rest_s && rest_s <= 30,
       "SNIPER_REST_S=" + g(rest_s) + " (seconds, 1 to 30)"},
      {rest_stop > 0, "SNIPER_REST_LIVE_STOP_USD=" + g(rest_stop) +
                          " (dollars, must be > 0)"},
      {1 <= maker_under_c && maker_under_c <= 10,
       "SNIPER_MAKER_UNDER_C=" + g(maker_under_c) +
           " (cents under the ask, 1 to 10)"},
      {0 <= maker_split && maker_split <= 0.9,
       "SNIPER_MAKER_SPLIT=" + g(maker_split) +
           " (a fraction of each order: 0 to 0.9)"},
      {!s.fair_no_limit || s.fair, "SNIPER_FAIR_NO_LIMIT needs SNIPER_FAIR=1"},
      {0 <= s.venue_min_shares && s.venue_min_shares <= 5 &&
           s.venue_min_shares == std::trunc(s.venue_min_shares),
       "SNIPER_VENUE_MIN_SHARES=" + g(s.venue_min_shares) +
           " (whole shares, 0 to 5; 0 = the venue's own)"},
      {s.venue_min_shares == 0 || s.order_sizing == "shares",
       "SNIPER_VENUE_MIN_SHARES needs SNIPER_ORDER_SIZING=shares"},
      {s.miss_retry == 0 || s.miss_retry == 1,
       "SNIPER_MISS_RETRY=" + std::to_string(s.miss_retry) +
           " (0 = off, 1 = one re-try after a confirmed miss)"},
      {s.miss_retry_ms > 0 && s.miss_retry_ms <= 5000,
       "SNIPER_MISS_RETRY_MS=" + g(s.miss_retry_ms) +
           " (must be > 0 and at most 5000)"},
      {s.miss_retry == 0 || s.order_type == "FAK",
       "SNIPER_MISS_RETRY needs SNIPER_ORDER_TYPE=FAK"},
      {!rest_live || (s.order_type == "FAK" && s.order_sizing == "shares"),
       "SNIPER_REST_LIVE needs SNIPER_ORDER_TYPE=FAK and "
       "SNIPER_ORDER_SIZING=shares (it rests a missed FAK's shares)"},
      {!(rest_live && s.miss_retry > 0),
       "SNIPER_REST_LIVE and SNIPER_MISS_RETRY: choose one (after a miss the "
       "order either rests or is sent again)"},
      {paper_cash_max_age_s > 0, "SNIPER_PAPER_CASH_MAX_AGE_S=" +
                                     g(paper_cash_max_age_s) +
                                     " (must be > 0)"},
      {delay_days > 0, "SNIPER_DELAY_DAYS=" + g(delay_days) + " (must be > 0)"},
      {30 <= fill_odds_orders && fill_odds_orders <= 100000 &&
           fill_odds_orders == std::trunc(fill_odds_orders),
       "SNIPER_FILL_ODDS_ORDERS=" + g(fill_odds_orders) +
           " (whole orders, 30 to 100000)"},
      {s.fair_vol_s >= 60, "SNIPER_FAIR_VOL_S=" + g(s.fair_vol_s) +
                               " (must be >= 60)"},
      {0 <= fair_fee_rate && fair_fee_rate < 0.25,
       "SNIPER_FAIR_FEE_RATE=" + g(fair_fee_rate) + " (must be 0 to 0.25)"},
      {0 < s.kelly_fraction && s.kelly_fraction <= 1,
       "SNIPER_KELLY_FRACTION=" + g(s.kelly_fraction) +
           " (must be above 0, at most 1)"},
      {0 <= s.min_entry_max_drawdown && s.min_entry_max_drawdown < 1,
       "SNIPER_MIN_ENTRY_MAX_DRAWDOWN=" + g(s.min_entry_max_drawdown) +
           " (a fraction: 0 to below 1)"},
      {0 <= dir_add && dir_add <= 1,
       "SNIPER_DIR_ADD=" + g(dir_add) +
           " (a fraction of the sniper's shares: 0 to 1)"},
      {0 < ptb_max_gap_s && ptb_max_gap_s <= 60,
       "SNIPER_PTB_MAX_GAP_S=" + g(ptb_max_gap_s) +
           " (seconds: above 0, at most 60)"},
      {0 <= s.ptb_margin_bp && s.ptb_margin_bp <= 50,
       "SNIPER_PTB_MARGIN_BP=" + g(s.ptb_margin_bp) + " (bp: 0 to 50)"},
      {1 <= ptb_min_checks && ptb_min_checks <= 24 &&
           ptb_min_checks == std::trunc(ptb_min_checks),
       "SNIPER_PTB_MIN_CHECKS=" + g(ptb_min_checks) +
           " (whole rounds, 1 to 24)"},
  };
  std::string bad;
  for (auto &[ok, msg] : checks)
    if (!ok)
      bad += (bad.empty() ? "" : "; ") + msg;
  if (!bad.empty())
    throw std::invalid_argument("invalid SNIPER_* settings: " + bad);
  return s;
}

SniperSettings sniper_settings(const std::map<std::string, std::string> &env,
                               std::vector<std::string> *notices) {
  return sniper_settings(
      [&](const std::string &k) -> std::optional<std::string> {
        auto it = env.find(k);
        if (it == env.end())
          return std::nullopt;
        return it->second;
      },
      notices);
}
} // namespace sniper
