// Missed-order diagnostics: classify every skipped signal, refused order and
// order outcome in a run's journal by reason code, and explain each one.
// Header-only; reads JSON rows, sends nothing, needs no keys.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sniper::miss {
using Json = nlohmann::json;

// What a reason means for the run. Only `software_failure` is a defect in
// the bot by itself; the other kinds are explained outcomes.
enum class Kind {
  StrategyFilter,  // an entry filter applied as designed
  RiskControl,     // sizing, caps, budgets, pauses applied as designed
  DataNotReady,    // a missed opportunity: required data or state missing
  LateOrStale,     // a missed opportunity: signal or book aged before send
  PriceMoved,      // an order that missed because the price moved
  NoLiquidity,     // an order that could not execute against the book
  VenueRejected,   // the exchange refused the order
  PaperModel,      // paper only: the simulator could not decide the fill
  SoftwareFailure, // a fault in the bot
  UnknownOutcome,  // an order whose result is not known
  DryRun,          // passed every check, deliberately not sent
  Unclassified,    // a reason this report does not know: inspect it
};

inline const char *kind_name(Kind k) {
  switch (k) {
  case Kind::StrategyFilter: return "strategy_filter";
  case Kind::RiskControl: return "risk_control";
  case Kind::DataNotReady: return "data_not_ready";
  case Kind::LateOrStale: return "late_or_stale";
  case Kind::PriceMoved: return "price_moved";
  case Kind::NoLiquidity: return "no_liquidity";
  case Kind::VenueRejected: return "venue_rejected";
  case Kind::PaperModel: return "paper_model";
  case Kind::SoftwareFailure: return "software_failure";
  case Kind::UnknownOutcome: return "unknown_outcome";
  case Kind::DryRun: return "dry_run";
  case Kind::Unclassified: return "unclassified";
  }
  return "unclassified";
}

inline const char *kind_verdict(Kind k) {
  switch (k) {
  case Kind::StrategyFilter: return "correct filter, not a bug";
  case Kind::RiskControl: return "correct risk control, not a bug";
  case Kind::DataNotReady: return "missed opportunity: data or state not ready";
  case Kind::LateOrStale: return "missed opportunity: aged before dispatch";
  case Kind::PriceMoved: return "unfilled: price moved past the limit";
  case Kind::NoLiquidity: return "unfilled: nothing to match at the limit";
  case Kind::VenueRejected: return "rejected by the exchange";
  case Kind::PaperModel: return "paper simulator could not decide (paper only)";
  case Kind::SoftwareFailure: return "software failure";
  case Kind::UnknownOutcome: return "unknown outcome: reconcile with the account";
  case Kind::DryRun: return "dry run: not sent by design";
  case Kind::Unclassified: return "unclassified: inspect";
  }
  return "unclassified: inspect";
}

struct Reason {
  std::string code;
  Kind kind = Kind::Unclassified;
  const char *advice = "";
};

// Reason text (prefix) -> stable code. Ordered: the first matching prefix wins.
struct Rule {
  const char *prefix, *code;
  Kind kind;
  const char *advice;
};
inline const std::vector<Rule> &rules() {
  static const std::vector<Rule> table{
      // entry filters
      {"ask not below fair value", "FILTER_ASK_NOT_BELOW_FAIR", Kind::StrategyFilter, "strategy edge filter; no action"},
      {"ask outside price band", "FILTER_PRICE_BAND", Kind::StrategyFilter, "MIN/MAX_BUY_PRICE band; no action"},
      {"before trade window", "FILTER_BEFORE_WINDOW", Kind::StrategyFilter, "TRADE_LAST_SECONDS window; no action"},
      {"too late in round", "FILTER_TOO_LATE", Kind::StrategyFilter, "minimum time left; no action"},
      {"too close to the price to beat", "FILTER_NEAR_PTB", Kind::StrategyFilter, "SNIPER_PTB_SKIP_CLOSE gate; no action"},
      {"fair limit under ask + SNIPER_MIN_GAP_C", "FILTER_MIN_GAP", Kind::StrategyFilter, "minimum fair gap; no action"},
      {"price cap at the price floor", "FILTER_PRICE_FLOOR", Kind::StrategyFilter, "cap below MIN_BUY_PRICE; no action"},
      {"fair value unknown: no move", "FILTER_NO_MOVE", Kind::StrategyFilter, "no BTC move; no action"},
      {"fair value unknown: round over", "FILTER_ROUND_OVER", Kind::StrategyFilter, "round ended; no action"},
      {"minimum entry already attempted", "FILTER_MIN_ENTRY_DONE", Kind::StrategyFilter, "one minimum entry per side and round; no action"},
      {"cooldown", "FILTER_COOLDOWN", Kind::StrategyFilter, "per-side cooldown; no action"},
      // sizing and risk
      {"kelly size under the", "RISK_KELLY_BELOW_MINIMUM", Kind::RiskControl, "Kelly size below the venue minimum; no action unless sizing is changed deliberately"},
      {"kelly room under venue minimum", "RISK_KELLY_ROOM", Kind::RiskControl, "remaining Kelly room below the minimum; no action"},
      {"kelly cash below signed order reservation", "RISK_CASH_RESERVED", Kind::RiskControl, "cash already reserved by open orders; no action"},
      {"share order is not a whole-cent amount", "RISK_WHOLE_CENT", Kind::RiskControl, "venue amount rule at this cap; no action"},
      {"round cap", "RISK_ROUND_CAP", Kind::RiskControl, "per-round fill or share cap; no action"},
      {"hybrid order risk cap", "RISK_HYBRID_CAP", Kind::RiskControl, "hybrid per-order risk cap; no action"},
      {"hybrid drawdown stop", "RISK_DRAWDOWN_STOP", Kind::RiskControl, "drawdown stop reached; review P&L before resuming"},
      {"previous order in flight", "RISK_ORDER_IN_FLIGHT", Kind::RiskControl, "one order per side at a time; no action"},
      {"awaiting FAK fill size", "RISK_AWAITING_FILL", Kind::RiskControl, "previous FAK result not known yet; no action"},
      {"live halted", "RISK_LIVE_HALTED", Kind::RiskControl, "a pause, budget or stop file held new orders; see detail"},
      {"STOP_LIVE present", "RISK_STOP_FILE", Kind::RiskControl, "stop file present; delete it to allow new orders"},
      {"kill switch stopped", "RISK_KILL_SWITCH", Kind::RiskControl, "bot stopping; no action"},
      {"canary", "RISK_CANARY_BUDGET", Kind::RiskControl, "live canary budget reached; no action"},
      {"collateral $", "RISK_INSUFFICIENT_BALANCE", Kind::RiskControl, "free collateral below the order cost incl. fee; fund the account or wait for settlements"},
      {"collateral fell below outstanding reservations", "RISK_COLLATERAL_RESERVED", Kind::RiskControl, "balance below open reservations; check the account"},
      {"collateral allowance insufficient", "RISK_ALLOWANCE", Kind::RiskControl, "token allowance too low; set the exchange allowance"},
      {"live orders are share-sized", "RISK_SIZING_MODE", Kind::RiskControl, "set SNIPER_ORDER_SIZING=shares for live"},
      {"unresolved native order from previous run", "RISK_INHERITED_ORDER", Kind::RiskControl, "an earlier order is unresolved; reconcile before new orders"},
      // data or state not ready
      {"no current round", "DATA_NO_ROUND", Kind::DataNotReady, "discovery had no current round; check discovery_refused rows and Gamma reachability"},
      {"book not live", "DATA_BOOK_NOT_LIVE", Kind::DataNotReady, "book unsynced (connect, gap or resync); check market websocket reconnects"},
      {"book stale", "DATA_BOOK_STALE", Kind::DataNotReady, "no book update within SNIPER_STALE_MS; check market websocket health"},
      {"fair value unknown: volatility warming up", "DATA_VOL_WARMUP", Kind::DataNotReady, "volatility window filling after start or reconnect; expected for the first minutes"},
      {"fair value unknown: no pre-move market history", "DATA_NO_BOOK_HISTORY", Kind::DataNotReady, "no book history before the move; expected right after a subscribe or resync"},
      {"fair value unknown: no market price", "DATA_NO_MARKET_PRICE", Kind::DataNotReady, "no mid on the book; check book depth"},
      {"fair value unknown: market fee rules not loaded", "DATA_FEE_RULES", Kind::DataNotReady, "market rules missing or older than 300 s; check discovery refreshes"},
      {"fair value unknown:", "DATA_FAIR_UNKNOWN", Kind::DataNotReady, "fair value inputs missing; see detail"},
      {"market rules not loaded", "DATA_RULES", Kind::DataNotReady, "market rules not cached yet; check discovery"},
      {"cash not read for this round yet", "DATA_CASH_NOT_READ", Kind::DataNotReady, "balance not sampled since the round began; check balance refresh"},
      {"fresh collateral balance unavailable", "DATA_BALANCE_STALE", Kind::DataNotReady, "balance older than 120 s; check the balance endpoint"},
      {"kelly size unknown: no cash reading", "DATA_NO_CASH", Kind::DataNotReady, "no bankroll reading yet"},
      {"venue minimum unavailable", "DATA_VENUE_MINIMUM", Kind::DataNotReady, "market minimum size not loaded"},
      {"hybrid risk unavailable", "DATA_HYBRID", Kind::DataNotReady, "hybrid accounting not ready or stale"},
      {"hybrid accounting", "DATA_HYBRID", Kind::DataNotReady, "hybrid accounting not ready or stale"},
      {"minimum entry state unavailable", "DATA_MIN_ENTRY_STATE", Kind::DataNotReady, "minimum-entry state not loaded"},
      {"private channel not ready", "DATA_PRIVATE_CHANNEL", Kind::DataNotReady, "user websocket not subscribed to this market; check user-channel reconnects"},
      {"native order connection not ready", "DATA_SENDER_NOT_READY", Kind::DataNotReady, "HTTP/2 order connection down; check transport reconnects"},
      {"native HTTP/2 sender not ready", "DATA_SENDER_NOT_READY", Kind::DataNotReady, "HTTP/2 order connection down; check transport reconnects"},
      {"coinbase disconnected", "DATA_TRIGGER_DISCONNECTED", Kind::DataNotReady, "Coinbase feed down; check coinbase_reconnect rows"},
      {"coinbase receive backlog", "DATA_TRIGGER_BACKLOG", Kind::DataNotReady, "Coinbase messages queued; check CPU and network load"},
      {"coinbase age unknown", "DATA_TRIGGER_AGE_UNKNOWN", Kind::DataNotReady, "trigger feed age unknown; check clock and feed"},
      {"coinbase feed behind", "DATA_TRIGGER_BEHIND", Kind::DataNotReady, "trigger feed behind its baseline; check network latency"},
      // timing: valid at decision, aged before dispatch
      {"trigger signal became stale", "LATE_TRIGGER_STALE", Kind::LateOrStale, "signal aged during checks or durable write; measure T4-T0"},
      {"decision book became stale", "LATE_BOOK_STALE", Kind::LateOrStale, "book aged during checks or durable write; measure T4-T2"},
      {"book became stale", "LATE_BOOK_STALE", Kind::LateOrStale, "book aged during checks or durable write; measure T4-T2"},
      {"outside the original decision window", "LATE_WINDOW", Kind::LateOrStale, "dispatch passed the decision window; measure T4-T0"},
      {"inside the matching-delay/expiry window", "LATE_EXPIRY", Kind::LateOrStale, "too close to round end at dispatch; no action"},
      {"market changed before dispatch", "LATE_ROUND_CHANGED", Kind::LateOrStale, "round switched between decision and dispatch; correct refusal"},
      {"fee rules changed after the fair-value decision", "LATE_RULES_CHANGED", Kind::LateOrStale, "fee rules refreshed mid-decision; correct refusal"},
      {"hybrid allocation tightened before dispatch", "LATE_HYBRID_TIGHTENED", Kind::LateOrStale, "risk room shrank before dispatch; correct refusal"},
      // execution against the book
      {"FAK no-fill", "LIQ_FAK_NO_FILL", Kind::NoLiquidity, "no ask at or below the limit at the match"},
      {"FAK no match", "LIQ_FAK_NO_MATCH", Kind::NoLiquidity, "exchange matched nothing at the limit"},
      {"no asks on this side", "LIQ_NO_ASKS", Kind::NoLiquidity, "empty ask side"},
      {"no asks left after our own recent fills", "LIQ_OWN_FILLS", Kind::NoLiquidity, "our fills consumed the asks"},
      {"makers pulled", "PRICE_LIVE_ODDS_MOVED", Kind::PriceMoved, "makers pulled before arrival"},
      {"venue refused native order", "VENUE_REFUSED", Kind::VenueRejected, "read venue_reply and http_status"},
      // paper simulator
      {"unknown book state at the modelled arrival", "PAPER_ARRIVAL_UNKNOWN", Kind::PaperModel, "strict arrival-book policy; SNIPER_BOOK_QUIET=1 relaxes it in paper only"},
      {"venue matching delay is unknown", "PAPER_DELAY_UNKNOWN", Kind::PaperModel, "no matching-delay estimate; paper only"},
      {"paper latency reached the round cutoff", "PAPER_CUTOFF", Kind::PaperModel, "modelled arrival after cutoff; paper only"},
      {"paper quote reached the round cutoff", "PAPER_CUTOFF", Kind::PaperModel, "modelled arrival after cutoff; paper only"},
      {"paper order is outside the current round", "PAPER_OUTSIDE_ROUND", Kind::PaperModel, "paper order outside its round; paper only"},
      {"paper market changed before order booking", "PAPER_ROUND_CHANGED", Kind::PaperModel, "round changed before paper booking; paper only"},
      {"paper orders on an inferred quiet-feed book", "PAPER_QUIET_BOOK", Kind::PaperModel, "paper fill on an inferred book; paper only"},
      // faults
      {"fault in native", "SW_HANDLER_FAULT", Kind::SoftwareFailure, "an exception escaped a handler; inspect the guard rows and logs"},
      {"audit or recorder failed", "SW_AUDIT_FAILED", Kind::SoftwareFailure, "journal or recorder write failed; check disk space"},
      {"journal disk failure", "SW_AUDIT_FAILED", Kind::SoftwareFailure, "journal write failed; check disk space"},
      {"order signing failed", "SW_SIGNING_FAILED", Kind::SoftwareFailure, "signing refused the order; inspect price and amount"},
      {"paper fill id collision", "SW_PAPER_ID", Kind::SoftwareFailure, "duplicate paper fill id"},
      {"dry run", "DRY_RUN", Kind::DryRun, "dry run; no action"},
  };
  return table;
}

inline Reason classify_reason(const std::string &why) {
  for (auto &r : rules())
    if (why.compare(0, std::string(r.prefix).size(), r.prefix) == 0)
      return {r.code, r.kind, r.advice};
  return {"UNCLASSIFIED", Kind::Unclassified, "unknown reason text; inspect"};
}

// The paper broker's own outcome codes.
inline std::optional<Reason> classify_paper_outcome(const std::string &o) {
  static const std::map<std::string, Reason> m{
      {"PRICE_CAP_MISS", {"PAPER_PRICE_CAP_MISS", Kind::PriceMoved, "ask rose above the cap before arrival"}},
      {"LIVE_ODDS_MISS", {"PAPER_LIVE_ODDS_MISS", Kind::PriceMoved, "odds moved before arrival"}},
      {"FOK_MISS_AT_WAKE", {"PAPER_FOK_MISS", Kind::PriceMoved, "book changed before arrival"}},
      {"MISS_AT_DECISION", {"PAPER_MISS_AT_DECISION", Kind::NoLiquidity, "nothing at the cap at decision"}},
      {"INSUFFICIENT_LIQUIDITY", {"PAPER_INSUFFICIENT_LIQUIDITY", Kind::NoLiquidity, "not enough size under the cap"}},
      {"SPREAD_TOO_WIDE", {"PAPER_SPREAD_TOO_WIDE", Kind::NoLiquidity, "spread too wide at arrival"}},
      {"BELOW_VENUE_MINIMUM", {"PAPER_BELOW_VENUE_MINIMUM", Kind::RiskControl, "order below the venue minimum"}},
      {"STALE_BOOK", {"PAPER_STALE_BOOK", Kind::LateOrStale, "book stale at arrival"}},
      {"UNKNOWN_BOOK_STATE", {"PAPER_ARRIVAL_UNKNOWN", Kind::PaperModel, "strict arrival-book policy"}},
  };
  auto it = m.find(o);
  if (it == m.end())
    return std::nullopt;
  return it->second;
}

struct Stats {
  std::vector<double> v;
  void add(double x) {
    if (std::isfinite(x) && x >= 0)
      v.push_back(x);
  }
  Json json() const {
    if (v.empty())
      return Json{{"n", 0}};
    auto s = v;
    std::sort(s.begin(), s.end());
    auto at = [&](double q) {
      auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(s.size())));
      return s[std::max<std::size_t>(rank, 1) - 1];
    };
    auto r = [](double x) { return std::round(x * 1000.0) / 1000.0; };
    return Json{{"n", s.size()}, {"p50", r(at(.5))}, {"p95", r(at(.95))},
                {"p99", r(at(.99))}, {"max", r(s.back())}};
  }
};

struct Order {
  std::string id, side, condition;
  double sent_wall = NAN, target = NAN;
  bool acknowledged = false, unknown = false, sent = false, timed = false;
  double matched = 0, confirmed = 0;
  std::optional<Reason> refusal;
  std::optional<double> first_fill_wall;
  std::string venue_reply;
  // These observations may only be joined inside their sending process run.
  std::uint64_t timing_run = 0;
  std::optional<std::int64_t> full_write_ns, confirmation_received_ns;
  std::string confirmation_source;
  bool write_conflict = false, confirmation_incomplete = false;
};

inline std::string text(const Json &row, const char *key) {
  auto it = row.find(key);
  return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}
// A boolean field; anything else (missing, string, number) is `fallback`.
inline bool flag(const Json &row, const char *key, bool fallback) {
  auto it = row.find(key);
  return it != row.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
inline std::optional<double> num(const Json &row, const char *key) {
  auto it = row.find(key);
  if (it == row.end() || !it->is_number())
    return std::nullopt;
  double x = it->get<double>();
  return std::isfinite(x) ? std::optional(x) : std::nullopt;
}
// Subtract integer monotonic stamps before converting the duration to double.
// An integral legacy JSON float is accepted only within the exact int64 range.
inline std::optional<std::int64_t> monotonic_ns(const Json &row, const char *key) {
  auto it = row.find(key);
  if (it == row.end())
    return std::nullopt;
  if (it->is_number_unsigned()) {
    auto value = it->get<std::uint64_t>();
    if (value > 0 && value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return static_cast<std::int64_t>(value);
  } else if (it->is_number_integer()) {
    auto value = it->get<std::int64_t>();
    if (value > 0)
      return value;
  } else if (it->is_number_float()) {
    const double value = it->get<double>();
    if (std::isfinite(value) && value > 0 && value < 0x1p63 && std::floor(value) == value)
      return static_cast<std::int64_t>(value);
  }
  return std::nullopt;
}

class Report {
public:
  void add(const Json &row) {
    ++rows_;
    if (!row.is_object()) {
      ++malformed_;
      return;
    }
    auto result = text(row, "result");
    if (result == "start") {
      flush_segment();
      ++runs_;
    } else if (result == "summary")
      last_summary_ = row;
    else if (result == "guard" || result == "coinbase_reconnect" ||
             result == "discovery_refused")
      disruptions_.push_back({num(row, "wall").value_or(NAN), result + ": " + text(row, "reason")});
    if (result == "skipped" || result == "refused" || result == "filled" ||
        result == "pending")
      ++decisions_[result];
    // Paper broker rows always carry the broker's `outcome`; live rows never do.
    bool paper = row.contains("outcome");
    if (result == "filled" || result == "refused" || (result == "pending" && !row.contains("acknowledged"))) {
      // Book age at the decision, for every order the engine decided to place.
      if (auto age = num(row, "dec_quote_age_ms"))
        stage_["book_age_at_decision"].add(*age);
      if (paper)
        if (auto lat = num(row, "latency_ms")) // paper: modelled, not measured
          stage_["paper_modelled_latency"].add(*lat);
    }
    if (result == "skipped")
      skipped(row);
    else if (paper && (result == "refused" || result == "filled"))
      outcome(row, result);
    if (!paper && row.contains("order_id"))
      live(row, result);
    else if (result == "live_fill" || result == "live_fill_update")
      ++orphan_fills_;
  }
  // Disruptions read from a recording: market (`k=mconn`) and Coinbase
  // (`k=cbconn`) websocket disconnects. Recording `t` is monotonic; only the
  // wall-clock `w` is comparable with journal rows, so an event without it is
  // not used.
  void add_record(const Json &e) {
    if (!e.is_object() || flag(e, "up", true))
      return;
    auto k = text(e, "k");
    auto w = num(e, "w");
    if (!w || (k != "mconn" && k != "cbconn"))
      return;
    disruptions_.push_back({*w, k == "mconn" ? "market websocket disconnected"
                                             : "coinbase websocket disconnected"});
  }
  Json json() {
    flush_segment();
    Json out;
    out["rows"] = rows_;
    out["malformed_rows"] = malformed_;
    out["runs"] = runs_;
    out["signals"] = signals_;
    out["decision_rows"] = decisions_;
    Json kinds = Json::object();
    auto put = [&](const Reason &r, std::uint64_t n, const char *source) {
      auto &k = kinds[kind_name(r.kind)];
      if (!k.contains("verdict")) {
        k["verdict"] = kind_verdict(r.kind);
        k["count"] = 0;
        k["codes"] = Json::object();
      }
      k["count"] = k["count"].get<std::uint64_t>() + n;
      auto &c = k["codes"][r.code];
      if (!c.contains("count")) {
        c["count"] = 0;
        c["advice"] = r.advice;
        c["source"] = source;
      }
      c["count"] = c["count"].get<std::uint64_t>() + n;
    };
    // Exact skip totals come from the bot's counters; journal rows are
    // rate-limited to one per second per reason, round and side.
    for (auto &[why, n] : skip_counts_)
      if (why != "live refused" && why != "live halted")
        put(classify_reason(why), n, "counter");
    for (auto &[why, n] : live_refusals_)
      put(classify_reason(why), n, "rows");
    for (auto &[code, e] : outcomes_)
      put(e.first, e.second, "rows");
    out["by_kind"] = kinds;
    Json un = Json::object();
    for (auto &[why, n] : unclassified_)
      un[why] = n;
    out["unclassified_reasons"] = un;
    out["evidence"] = evidence_;
    // orders
    Json o{{"submitted", 0}, {"acknowledged", 0}, {"filled", 0},
           {"partially_filled", 0}, {"matched_unconfirmed", 0},
           {"unfilled", 0}, {"rejected", 0}, {"unknown_outcome", 0},
           {"no_reply_yet", 0}};
    Stats to_fill, websocket_confirmation, reconciliation_confirmation;
    for (auto &[id, ord] : orders_) {
      if (!ord.sent)
        continue;
      auto inc = [&](const char *k) { o[k] = o[k].get<int>() + 1; };
      inc("submitted");
      if (ord.acknowledged)
        inc("acknowledged");
      double tgt = std::isfinite(ord.target) ? ord.target : 0;
      if (ord.confirmed > 0)
        inc(tgt > 0 && ord.confirmed + 1e-6 < tgt ? "partially_filled" : "filled");
      else if (ord.matched > 0)
        inc("matched_unconfirmed");
      else if (ord.unknown)
        inc("unknown_outcome");
      else if (ord.refusal && ord.refusal->kind == Kind::VenueRejected)
        inc("rejected");
      else if (ord.refusal)
        inc("unfilled");
      else
        inc("no_reply_yet");
      if (ord.first_fill_wall && std::isfinite(ord.sent_wall))
        to_fill.add((*ord.first_fill_wall - ord.sent_wall) * 1000.0);
      if (!ord.write_conflict && !ord.confirmation_incomplete &&
          ord.full_write_ns && ord.confirmation_received_ns &&
          *ord.confirmation_received_ns >= *ord.full_write_ns) {
        const double elapsed = static_cast<double>(*ord.confirmation_received_ns -
                                                    *ord.full_write_ns) / 1e6;
        if (ord.confirmation_source == "websocket")
          websocket_confirmation.add(elapsed);
        else if (ord.confirmation_source == "rest_reconciliation")
          reconciliation_confirmation.add(elapsed);
      }
    }
    o["paper_orders"] = paper_orders_;
    o["orphan_fill_rows"] = orphan_fills_;
    out["orders"] = o;
    Json lat;
    for (auto &[k, s] : stage_)
      lat[k] = s.json();
    if (!lat.contains("pre_send_checks"))
      lat["pre_send_checks"] = Stats{}.json();
    lat["send_to_first_fill_ms"] = to_fill.json();
    lat["send_to_websocket_confirmation_evidence_ms"] = websocket_confirmation.json();
    lat["send_to_reconciliation_confirmation_evidence_ms"] = reconciliation_confirmation.json();
    out["latency_ms"] = lat;
    out["latency_evidence"] = {
        {"legacy_trigger_to_dispatch_includes_checks", legacy_dispatch_fallbacks_},
        {"pre_send_checks_missing_boundary", missing_check_boundaries_},
        {"trigger_to_dispatch_note", "T0 to T1; legacy rows without T1 retain the T0-to-T2 fallback, which includes checks"},
        {"confirmation_note", "first recorded positive CONFIRMED receipt per order, joined only within its dispatch run; client receipt time is not venue execution time"},
        {"legacy_fill_note", "send_to_first_fill_ms retains the legacy dispatch/fill wall-clock rows, not full-wire monotonic timing"},
        {"request_write_note", "T4 to T5 includes final guards, headers, enqueueing and complete socket write"},
        {"network_and_ack_note", "includes network and venue response time; cannot isolate an exchange-imposed delay"}};
    Json cause{{"missed_with_disruption_within_30s", near_disruption_},
               {"missed_without_disruption", missed_ - near_disruption_},
               {"note", "counts journal rows (rate-limited); a disruption shortly before a data or timing miss is a suspected cause, not a confirmed one"}};
    out["suspected_causes"] = cause;
    return out;
  }

private:
  std::uint64_t rows_ = 0, malformed_ = 0, runs_ = 0, orphan_fills_ = 0,
                missed_ = 0, near_disruption_ = 0;
  double signals_ = 0;
  std::uint64_t legacy_dispatch_fallbacks_ = 0, missing_check_boundaries_ = 0;
  std::map<std::string, std::uint64_t> decisions_, skip_counts_, live_refusals_, unclassified_;
  std::map<std::string, std::pair<Reason, std::uint64_t>> outcomes_;
  std::map<std::string, Order> orders_;
  std::map<std::string, Stats> stage_;
  std::vector<std::pair<double, std::string>> disruptions_;
  Json evidence_ = Json::object(), last_summary_;
  Json paper_orders_{{"filled", 0}, {"partially_filled", 0}, {"refused", 0}};

  void flush_segment() {
    if (!last_summary_.is_object())
      return;
    signals_ += num(last_summary_, "signals").value_or(0);
    if (auto c = last_summary_.find("counters"); c != last_summary_.end() && c->is_object())
      for (auto &[k, v] : c->items())
        if (k.rfind("skip: ", 0) == 0 && v.is_number_unsigned())
          skip_counts_[k.substr(6)] += v.get<std::uint64_t>();
    last_summary_ = Json();
  }
  void note(const Reason &r, const std::string &why, const Json &row) {
    if (r.kind == Kind::Unclassified)
      ++unclassified_[why.substr(0, 160)];
    auto &ev = evidence_[r.code];
    if (!ev.is_array())
      ev = Json::array();
    if (ev.size() < 3) {
      Json e{{"reason", why.substr(0, 200)}};
      for (const char *k : {"wall", "round_end", "condition_id", "side", "id", "order_id",
                            "left_s", "dec_quote_age_ms", "coinbase_excess_ms", "detail",
                            "http_status", "venue_reply"})
        if (row.contains(k))
          e[k] = row.at(k);
      ev.push_back(std::move(e));
    }
    if (r.kind == Kind::DataNotReady || r.kind == Kind::LateOrStale) {
      ++missed_;
      double w = num(row, "wall").value_or(NAN);
      for (auto &[t, what] : disruptions_)
        if (std::isfinite(w) && t <= w && w - t <= 30.0) {
          ++near_disruption_;
          break;
        }
    }
  }
  void skipped(const Json &row) {
    auto why = text(row, "reason");
    if (why == "live refused" || why == "live halted") {
      auto detail = text(row, "detail");
      auto r = classify_reason(detail);
      if (why == "live halted" && r.kind == Kind::Unclassified)
        r = classify_reason("live halted");
      ++live_refusals_[detail.empty() ? why : detail];
      note(r, detail.empty() ? why : detail, row);
    } else
      note(classify_reason(why), why, row);
  }
  void outcome(const Json &row, const std::string &result) {
    auto oc = text(row, "outcome");
    auto why = text(row, "reason");
    if (result == "filled") {
      double got = num(row, "shares").value_or(0), want = num(row, "order_shares").value_or(0);
      auto k = want > 0 && got + 1e-6 < want ? "partially_filled" : "filled";
      paper_orders_[k] = paper_orders_[k].get<int>() + 1;
      return;
    }
    paper_orders_["refused"] = paper_orders_["refused"].get<int>() + 1;
    // The broker's outcome code is more precise than the reason text (it
    // separates a price that moved from missing size); TECHNICAL_ERROR is its
    // catch-all, so the reason text decides then.
    auto p = classify_paper_outcome(oc);
    auto r = p ? *p : classify_reason(why);
    auto &e = outcomes_[r.code];
    e.first = r;
    ++e.second;
    note(r, why.empty() ? oc : why, row);
  }
  void live(const Json &row, const std::string &result) {
    auto id = text(row, "order_id");
    if (id.empty())
      return;
    auto &o = orders_[id];
    o.id = id;
    auto ack = row.find("acknowledged");
    bool is_reply = ack != row.end();
    if (result == "pending" && !is_reply) { // the dispatch row
      if (!o.sent || o.timing_run != runs_) {
        o.timing_run = runs_;
        o.full_write_ns.reset();
        o.confirmation_received_ns.reset();
        o.confirmation_source.clear();
        o.write_conflict = o.confirmation_incomplete = false;
      }
      o.sent = true;
      o.side = text(row, "side");
      o.condition = text(row, "condition_id");
      o.sent_wall = num(row, "wall").value_or(NAN);
      o.target = num(row, "order_shares").value_or(num(row, "shares").value_or(NAN));
    }
    if (o.sent && o.timing_run == runs_)
      if (auto write = monotonic_ns(row, "T5_ns")) {
        if (o.full_write_ns && *o.full_write_ns != *write)
          o.write_conflict = true;
        else
          o.full_write_ns = write;
      }
    if (is_reply && ack->is_boolean()) {
      if (ack->get<bool>())
        o.acknowledged = true;
      else if (flag(row, "never_retry", false))
        o.unknown = true;
    }
    if (is_reply) {
      auto ns = [&](const char *k) { return monotonic_ns(row, k); };
      auto t0 = ns("T0_ns"), t1 = ns("T1_ns"), t2 = ns("T2_ns"), t3 = ns("T3_ns"), t4 = ns("T4_ns"),
           t5 = ns("T5_ns"), t6 = ns("T6_ns");
      auto span = [&](const char *name, std::optional<std::int64_t> a,
                       std::optional<std::int64_t> b) {
        if (a && b && *b >= *a && *a > 0)
          stage_[name].add(static_cast<double>(*b - *a) / 1e6);
      };
      if (o.acknowledged && !o.timed) {
        o.timed = true;
        if (row.contains("T1_ns")) {
          span("trigger_to_dispatch", t0, t1);
          span("pre_send_checks", t1, t2);
          if (!t1 || !t2 || *t2 < *t1)
            ++missing_check_boundaries_;
        } else {
          span("trigger_to_dispatch", t0, t2);
          if (t0 && t2 && *t2 >= *t0)
            ++legacy_dispatch_fallbacks_;
          ++missing_check_boundaries_;
        }
        span("signing", t2, t3);
        span("durable_intent", t3, t4);
        span("request_write", t4, t5);
        span("network_and_ack", t5, t6);
        span("trigger_to_ack", t0, t6);
      }
    }
    if (result == "refused") {
      auto why = text(row, "reason");
      auto r = classify_reason(why);
      o.refusal = r;
      o.venue_reply = text(row, "venue_reply");
      auto &e = outcomes_[r.code];
      e.first = r;
      ++e.second;
      note(r, why, row);
    } else if (result == "live_fill_update") {
      o.matched = std::max(o.matched, num(row, "shares").value_or(0));
    } else if (result == "live_fill") {
      o.confirmed += num(row, "shares").value_or(0);
      if (!o.first_fill_wall)
        o.first_fill_wall = num(row, "wall");
      // Reconciliation can be posted after a later websocket callback. Select
      // the first monotonic receipt, not the order in which rows were written.
      if (o.sent && o.timing_run == runs_ && text(row, "status") == "CONFIRMED" &&
          num(row, "shares").value_or(0) > 0) {
        auto receipt = monotonic_ns(row, "evidence_received_ns");
        auto source = text(row, "evidence_source");
        if (!receipt || (source != "websocket" && source != "rest_reconciliation"))
          o.confirmation_incomplete = true;
        else if (!o.confirmation_received_ns || *receipt < *o.confirmation_received_ns) {
          o.confirmation_received_ns = receipt;
          o.confirmation_source = source;
        } else if (*receipt == *o.confirmation_received_ns && source != o.confirmation_source)
          o.confirmation_incomplete = true;
      }
    } else if (result == "pending" && is_reply && o.unknown) {
      Reason r{"UNKNOWN_ORDER_OUTCOME", Kind::UnknownOutcome, "never retried; reconcile with the account trades"};
      auto &e = outcomes_[r.code];
      e.first = r;
      ++e.second;
      note(r, text(row, "reason"), row);
    }
  }
};

// Feeds every JSON line of `in` to `use`; returns the number of malformed lines.
template <class F> std::uint64_t read_lines(std::istream &in, F use) {
  std::uint64_t bad = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    auto j = Json::parse(line, nullptr, false);
    if (j.is_discarded()) {
      ++bad;
      continue;
    }
    use(j);
  }
  return bad;
}
} // namespace sniper::miss
