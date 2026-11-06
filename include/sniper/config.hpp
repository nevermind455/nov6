#pragma once
#include "sniper.hpp"
#include "types.hpp"
#include <filesystem>
#include <set>
namespace sniper {
struct Config {
  std::filesystem::path root, state, journal, ledger, stop_file, record;
  bool live = false, health = false, new_session = false, check = false,
       dash = false, dry_run = false, canary_hard_stop = true;
  double dash_fps = 2;
  int conns = 2, max_fills = 20, backlog_max = 20, guard_strikes = 3;
  double k = .5, window = 250, cooldown = 300, min_left = 10, stale_ms = 2000,
         max_side = 200;
  double min_price = .05, max_price = .95, slip = .05, margin = .01,
         vol_window = 300, kelly = .25;
  double coinbase_stale = 1000, coinbase_unknown = 5000, silent = 5,
         trade_last = 300, min_expiry = 10;
  bool fair = true, kelly_on = true, hybrid = true, flat = false,
       min_entry = false, fair_no_limit = false;
  bool ptb_skip = false, settle_onchain = true;
  double ptb_margin = 1, min_gap = 0, baseline_bp = 1, dd_stop = .2,
         dd_pause = 600, min_entry_dd = 0;
  Units risk_fraction = 0, max_order = 0, start_cash = 1000 * U,
        venue_min_override = 0;
  int shares = 10, hold_ms = 250, extra_ms = 200, book_wait_ms = 500,
      send_timeout_s = 10;
  double run_for = 0, guard_window = 1800;
  Units canary_shares = 25, canary_orders = 40, canary_fills = 20,
        canary_rounds = 12, canary_spend = 60 * U, canary_loss = 20 * U;
  std::string order_type = "FAK",
              rpc = "https://polygon-bor-rpc.publicnode.com";
  std::string private_key, api_key, api_secret, passphrase, funder;
  int signature_type = 0;
  std::vector<std::string> notices;
  SniperSettings s; // sniper_mode.Settings, parsed as the reference does
  std::map<std::string, std::string> values;
  std::set<std::string> used;
  static Config load(int argc, char **argv);
  void validate() const;
  Json public_view() const;
  std::string text(const std::string &, const std::string &);
  double numeric(const std::string &, double, double, double,
                 bool whole = false);
  bool flag(const std::string &, bool);
  // The raw value of a setting (process environment first, then .env), or
  // nullopt when unset; marks it used.
  std::optional<std::string> raw(const std::string &);
};
} // namespace sniper
