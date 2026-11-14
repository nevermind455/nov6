#pragma once
#include "sniper.hpp"
#include "types.hpp"
#include <filesystem>
#include <set>
namespace sniper {
struct Config {
  std::filesystem::path root, state, journal, paper_journal, ledger, stop_file, record;
  bool live = false, health = false, new_session = false, check = false,
       dash = false, dry_run = false, canary_hard_stop = true;
  double dash_fps = 2;
  int guard_strikes = 3;
  double min_price = .05, max_price = .95, trade_last = 300, min_expiry = 10;
  bool settle_onchain = true;
  // A second, warm connection to the venue's market feed that takes over at
  // once when the first drops (SNIPER_MARKET_STANDBY, on by default).
  bool market_standby = true;
  Units start_cash = 1000 * U;
  int send_timeout_s = 10;
  double run_for = 0, guard_window = 1800;
  // Seconds before a round boundary at which the next round's books are
  // subscribed and its rules cached (SNIPER_PREPARE_LEAD_S).
  double prepare_lead = 60;
  Units canary_shares = 25, canary_orders = 40, canary_fills = 20,
        canary_rounds = 12, canary_spend = 60 * U, canary_loss = 20 * U;
  std::string rpc = "https://polygon-bor-rpc.publicnode.com";
  std::string private_key, api_key, api_secret, passphrase, funder;
  int signature_type = 0;
  std::vector<std::string> notices;
  // sniper_mode.Settings, parsed as the reference does: the one copy of the
  // trading settings (trigger, sizing, limits, feeds) the runtime reads.
  SniperSettings s;
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
