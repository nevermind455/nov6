#include "sniper/config.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
namespace sniper {
static std::string trim(std::string s) {
  auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == s.npos ? "" : s.substr(a, b - a + 1);
}
std::string Config::text(const std::string &name, const std::string &fallback) {
  used.insert(name);
  if (auto p = std::getenv(name.c_str()))
    return trim(p);
  auto i = values.find(name);
  return i == values.end() ? fallback : i->second;
}
std::optional<std::string> Config::raw(const std::string &name) {
  used.insert(name);
  if (auto p = std::getenv(name.c_str()))
    return std::string(p);
  auto i = values.find(name);
  if (i == values.end())
    return std::nullopt;
  return i->second;
}
double Config::numeric(const std::string &name, double fallback, double low,
                       double high, bool whole) {
  auto s = text(name, fmt::format("{}", fallback));
  std::size_t n = 0;
  double x;
  try {
    x = std::stod(s, &n);
  } catch (...) {
    throw std::invalid_argument(name + " must be a finite number");
  }
  if (n != s.size() || !std::isfinite(x) || x < low || x > high ||
      (whole && std::floor(x) != x))
    throw std::invalid_argument(name + " is out of range");
  return x;
}
bool Config::flag(const std::string &name, bool fallback) {
  auto s = text(name, fallback ? "1" : "0");
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (s == "1" || s == "true" || s == "yes" || s == "on")
    return true;
  if (s == "0" || s == "false" || s == "no" || s == "off")
    return false;
  throw std::invalid_argument(name + " must be 1 or 0");
}
Config Config::load(int argc, char **argv) {
  Config c;
  bool requested_paper = false, explicit_config = false;
  c.root = std::filesystem::current_path();
  std::filesystem::path env_path = c.root / ".env";
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--live")
      c.live = true;
    else if (a == "--paper") {
      requested_paper = true;
    } else if (a == "--health")
      c.health = true;
    else if (a == "--check")
      c.check = true;
    else if (a == "--new-canary-session")
      c.new_session = true;
    else if (a == "--config" && i + 1 < argc) {
      env_path = std::filesystem::absolute(argv[++i]);
      c.root = env_path.parent_path();
      explicit_config = true;
    } else if (a == "--run-for" && i + 1 < argc) {
      std::size_t consumed = 0;
      std::string seconds = argv[++i];
      c.run_for = std::stod(seconds, &consumed);
      if (consumed != seconds.size() || !std::isfinite(c.run_for) ||
          c.run_for <= 0)
        throw std::invalid_argument("--run-for must be positive");
    } else if (a == "--record" && i + 1 < argc)
      c.record = argv[++i];
    else
      throw std::invalid_argument("unknown or incomplete argument: " + a);
  }
  if (requested_paper && c.live)
    throw std::invalid_argument("--paper and --live cannot be combined");
  if (c.health && c.live)
    throw std::invalid_argument("--health and --live cannot be combined");
  struct stat st {};
  if (lstat(env_path.c_str(), &st) == 0) {
    if (!S_ISREG(st.st_mode) || (st.st_mode & 077))
      throw std::invalid_argument(".env must be a regular file with mode 600");
    std::ifstream f(env_path);
    std::string line;
    while (std::getline(f, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#')
        continue;
      if (line.rfind("export ", 0) == 0)
        line = trim(line.substr(7));
      auto equal = line.find('=');
      if (equal == line.npos)
        throw std::invalid_argument("invalid .env line");
      auto key = trim(line.substr(0, equal)),
           value = trim(line.substr(equal + 1));
      if (value.size() >= 2 &&
          ((value.front() == '\'' && value.back() == '\'') ||
           (value.front() == '\"' && value.back() == '\"')))
        value = value.substr(1, value.size() - 2);
      c.values[key] = value;
    }
  } else if (explicit_config)
    throw std::invalid_argument("--config file not found: " + env_path.string());
  for (auto key : {"PRIVATE_KEY", "POLY_PRIVATE_KEY", "POLY_API_KEY",
                   "POLY_API_SECRET", "POLY_API_PASSPHRASE"})
    if (c.values.count(key))
      throw std::invalid_argument(
          std::string(key) +
          " must be exported in the environment, not stored in .env");
  if (!c.flag("SNIPER_ONLY", true))
    throw std::invalid_argument("SNIPER_ONLY must be 1");
  bool canary = c.flag("SNIPER_LIVE_CANARY", false);
  if (c.live && !canary)
    throw std::invalid_argument("--live requires SNIPER_LIVE_CANARY=1");
  c.min_price = c.numeric("MIN_BUY_PRICE", .05, 0, .99);
  c.max_price = c.numeric("MAX_BUY_PRICE", .95, .001, .9999);
  c.trade_last = c.numeric("TRADE_LAST_SECONDS", 300, 1, 300);
  c.min_expiry = c.numeric("MIN_SECONDS_TO_EXPIRY", 10, 0, 299);
  c.send_timeout_s = c.numeric("SNIPER_SEND_TIMEOUT_S", 10, 1, 60, true);
  c.guard_strikes = c.numeric("SNIPER_GUARD_STRIKES", 3, 1, 100, true);
  c.guard_window = c.numeric("SNIPER_GUARD_WINDOW_S", 1800, 1, 86400);
  c.canary_shares = c.numeric("SNIPER_CANARY_MAX_SHARES", 25, 0, 100000, true);
  c.canary_orders = c.numeric("SNIPER_CANARY_MAX_ORDERS", 40, 0, 100000, true);
  c.canary_fills = c.numeric("SNIPER_CANARY_MAX_FILLS", 20, 0, 100000, true);
  c.canary_rounds = c.numeric("SNIPER_CANARY_MAX_ROUNDS", 12, 0, 100000, true);
  c.canary_spend =
      units(fixed6(c.numeric("SNIPER_CANARY_MAX_SPEND_USD", 60, 0, 1e9)));
  c.canary_loss =
      units(fixed6(c.numeric("SNIPER_CANARY_MAX_LOSS_USD", 20, 0, 1e9)));
  c.settle_onchain = c.flag("SETTLE_ONCHAIN", true);
  c.rpc = c.text("POLYGON_RPC", "https://polygon-bor-rpc.publicnode.com");
  c.start_cash = units(c.text("PAPER_START_BALANCE", "1000"));
  c.state = c.root / "state" / (c.live ? "live" : "paper");
  c.ledger = c.text(c.live ? "LEDGER_PATH" : "PAPER_LEDGER_PATH",
                    (c.state / "ledger.json").string());
  c.journal = c.text(c.live ? "LIVE_SNIPER_JOURNAL" : "PAPER_AUDIT_PATH",
                     (c.state / "sniper.jsonl").string());
  c.stop_file =
      c.text("SNIPER_CANARY_STOP_FILE", (c.root / "STOP_LIVE").string());
  for (auto p : {&c.ledger, &c.journal, &c.stop_file, &c.record})
    if (!p->empty() && p->is_relative())
      *p = c.root / *p;
  auto secret = [](const char *key) {
    auto p = std::getenv(key);
    return p ? std::string(p) : std::string{};
  };
  if (c.live) {
    c.private_key = secret("POLY_PRIVATE_KEY");
    if (c.private_key.empty())
      c.private_key = secret("PRIVATE_KEY");
    c.api_key = secret("POLY_API_KEY");
    c.api_secret = secret("POLY_API_SECRET");
    c.passphrase = secret("POLY_API_PASSPHRASE");
    c.funder = c.text("POLY_FUNDER", "");
    c.signature_type = c.numeric("POLY_SIGNATURE_TYPE", 0, 0, 2, true);
  }
  c.s = sniper_settings(
      [&c](const std::string &name) { return c.raw(name); }, &c.notices);
  // the runtime's own copies of the reference settings
  c.k = c.s.k_bp;
  c.window = c.s.window_ms;
  c.cooldown = c.s.cooldown_ms;
  c.min_left = c.s.min_left_s;
  c.stale_ms = c.s.stale_ms;
  c.max_fills = c.s.max_fills_round;
  c.max_side = c.s.max_side_shares;
  c.slip = c.s.slip;
  c.fair = c.s.fair;
  c.margin = c.s.fair_margin;
  c.vol_window = c.s.fair_vol_s;
  c.min_gap = c.s.min_gap_c;
  c.fair_no_limit = c.s.fair_no_limit;
  c.kelly_on = c.s.kelly;
  c.kelly = c.s.kelly_fraction;
  c.hybrid = c.s.hybrid_sizing;
  c.flat = c.s.flat_live;
  c.order_type = c.s.order_type;
  c.conns = c.s.coinbase_conns;
  c.shares = static_cast<int>(c.s.shares);
  c.baseline_bp = c.s.hybrid_policy.vol_baseline_bp;
  c.dd_stop = c.s.hybrid_policy.drawdown_stop;
  c.dd_pause = c.s.hybrid_dd_pause_s;
  c.risk_fraction = units(fixed6(c.s.hybrid_policy.risk_fraction));
  c.max_order = units(fixed6(c.s.hybrid_policy.max_order_usd));
  c.min_entry = c.s.kelly_min_entry;
  c.min_entry_dd = c.s.min_entry_max_drawdown;
  c.venue_min_override = static_cast<Units>(c.s.venue_min_shares) * U;
  c.coinbase_stale = c.s.coinbase_stale_ms;
  c.coinbase_unknown = c.s.coinbase_unknown_ms;
  c.backlog_max = c.s.coinbase_backlog_max;
  c.silent = c.s.coinbase_silent_s;
  c.hold_ms = static_cast<int>(c.s.hold_ms);
  c.extra_ms = static_cast<int>(c.s.paper_extra_ms);
  c.book_wait_ms = static_cast<int>(c.s.book_wait_ms);
  c.ptb_skip = c.s.ptb_skip_close;
  c.ptb_margin = c.s.ptb_margin_bp;
  // The runtime reads these later. They count as used here, and --check refuses
  // a bad value now instead of leaving it to fail at the real start.
  for (auto name :
       {"MAX_ALLOWED_SPREAD", "PAPER_LATENCY_MS", "ORDERBOOK_MAX_AGE_SECONDS",
        "ORDERBOOK_MAX_QUIET_SECONDS", "ORDERBOOK_FUTURE_TOLERANCE_SECONDS",
        "PAPER_FLAGGED_DELAY_SECONDS", "PAPER_OWN_FILL_TTL_S"}) {
    auto v = c.raw(name);
    if (!v || v->find_first_not_of(" \t") == std::string::npos)
      continue;
    double x;
    try {
      x = py::parse_float(*v);
    } catch (const py::ValueError &) {
      throw std::invalid_argument(std::string(name) +
                                  " must be a finite number");
    }
    if (!std::isfinite(x))
      throw std::invalid_argument(std::string(name) +
                                  " must be a finite number");
  }
  for (auto &[name, value] : c.values)
    if (!c.used.count(name))
      c.notices.push_back("unused setting: " + name);
  c.validate();
  return c;
}
void Config::validate() const {
  if (start_cash <= 0 || min_price >= max_price || min_expiry >= trade_last)
    throw std::invalid_argument("invalid cash, price band or trading window");
  if ((min_gap > 0 || fair_no_limit) && !fair)
    throw std::invalid_argument(
        "SNIPER_FAIR_NO_LIMIT and SNIPER_MIN_GAP_C require SNIPER_FAIR=1");
  if (new_session && !live)
    throw std::invalid_argument("--new-canary-session requires --live");
  if (live) {
    if (private_key.empty())
      throw std::invalid_argument("POLY_PRIVATE_KEY is required for --live");
    if (signature_type > 0 && funder.empty())
      throw std::invalid_argument(
          "POLY_FUNDER is required for proxy signature types");
    if (k != .5 || window != 250 || cooldown != 300 || slip != .05 || !fair ||
        margin != .01 || order_type != "FAK" || fair_no_limit)
      throw std::invalid_argument(
          "live frozen-spec check failed: trigger, window, cooldown, slip, "
          "fair margin and FAK must match the PDF baseline");
    if (!(kelly_on && hybrid && kelly >= .25 && kelly <= .5) &&
        !(flat && !kelly_on && shares >= 5 && shares <= 20))
      throw std::invalid_argument(
          "live frozen-spec check failed: Kelly/hybrid or flat live sizing");
    if (canary_shares < shares)
      throw std::invalid_argument(
          "SNIPER_CANARY_MAX_SHARES must cover SNIPER_SHARES");
    if (s.order_sizing != "shares")
      throw std::invalid_argument(
          "live orders are share-sized in this build: set "
          "SNIPER_ORDER_SIZING=shares");
  }
}
Json Config::public_view() const {
  return {{"native_cpp", true},
          {"mode", live ? "LIVE" : "PAPER"},
          {"trigger_bp", k},
          {"window_ms", window},
          {"order_type", order_type},
          {"kelly_fraction", kelly},
          {"fair_margin", margin},
          {"runtime_dependencies", "C++ libraries; no Python"},
          {"independent_paper_model", !live},
          {"notices", notices}};
}
} // namespace sniper
