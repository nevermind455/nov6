// sniper-live-check: a short health report of a run - halts, errors, orders,
// fills, resting orders, the trigger feeds, websockets, settlement and the
// ledger. Read-only: it opens the journal and the ledger snapshot, sends
// nothing, needs no key and prints no secret (ids are shortened). It runs at
// the lowest CPU priority, so it never competes with a bot on the same machine.
//
//   sniper-live-check                      state/live, the last 24 hours
//   sniper-live-check --hours 6
//   sniper-live-check --state state/paper
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr long long U = 1000000;

std::string sf(const char *f, auto... a) {
  char b[512];
  int n = std::snprintf(b, sizeof b, f, a...);
  return n < 0 ? std::string() : std::string(b, std::min<std::size_t>(n, sizeof b - 1));
}
std::string hm(double wall) {
  if (!(wall > 0))
    return "--";
  std::time_t t = static_cast<std::time_t>(wall);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, "%m-%d %H:%M", &tm);
  return b;
}
std::string dur(double seconds) {
  long long s = static_cast<long long>(std::max(0.0, seconds));
  if (s >= 3600)
    return sf("%lldh%02lldm", s / 3600, s % 3600 / 60);
  if (s >= 60)
    return sf("%lldm%02llds", s / 60, s % 60);
  return sf("%llds", s);
}
std::string str(const Json &v) {
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_null())
    return "";
  return v.dump();
}
std::string shorten(const std::string &t, std::size_t n = 90) {
  return t.size() <= n ? t : t.substr(0, n - 1) + "~";
}
double num(const Json &v, double fallback = 0) {
  if (v.is_number())
    return v.get<double>();
  if (v.is_string())
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
    }
  return fallback;
}
const Json &at(const Json &o, const char *k) {
  static const Json null;
  if (!o.is_object())
    return null;
  auto it = o.find(k);
  return it == o.end() ? null : *it;
}
std::optional<double> pct(std::vector<double> v, double q) {
  if (v.empty())
    return std::nullopt;
  std::sort(v.begin(), v.end());
  long i = static_cast<long>(std::ceil(q / 100 * static_cast<double>(v.size()))) - 1;
  return v[static_cast<std::size_t>(std::clamp(i, 0L, static_cast<long>(v.size()) - 1))];
}
std::string money(long long units) { return sf("$%+.2f", static_cast<double>(units) / U); }

// A counter that keeps first-seen order for ties, like Python's most_common.
struct Tally {
  std::vector<std::pair<std::string, long long>> items;
  void add(const std::string &k, long long n = 1) {
    for (auto &[key, v] : items)
      if (key == k) {
        v += n;
        return;
      }
    items.emplace_back(k, n);
  }
  long long get(const std::string &k) const {
    for (auto &[key, v] : items)
      if (key == k)
        return v;
    return 0;
  }
  long long total() const {
    long long t = 0;
    for (auto &[k, v] : items)
      t += v;
    return t;
  }
  std::vector<std::pair<std::string, long long>> top(std::size_t n = 1000) const {
    auto v = items;
    std::stable_sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
    if (v.size() > n)
      v.resize(n);
    return v;
  }
  bool empty() const { return items.empty(); }
};
std::string joined(const std::vector<std::pair<std::string, long long>> &v, const char *suffix = "") {
  std::string out;
  for (auto &[k, n] : v)
    out += (out.empty() ? "" : ", ") + k + " " + std::to_string(n) + suffix;
  return out;
}

struct Run {
  double start = 0;
  std::optional<double> end;
  bool final = false;
  Json exit_code;
};
struct Error {
  long long n = 0;
  double first = 0, last = 0;
  std::string extra;
};
} // namespace

int main(int argc, char **argv) {
  std::string state = "state/live";
  double hours = 24;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--state" && i + 1 < argc)
      state = argv[++i];
    else if (a == "--hours" && i + 1 < argc)
      hours = std::stod(argv[++i]);
    else {
      std::cerr << "usage: sniper-live-check [--state state/live] [--hours 24]\n";
      return 2;
    }
  }
  if (::nice(19) == -1) {
  } // never compete with a bot running on the same machine
  namespace fs = std::filesystem;
  const fs::path journal = fs::path(state) / "sniper.jsonl", ledger_path = fs::path(state) / "ledger.json";
  if (!fs::exists(journal)) {
    std::cerr << "no journal at " << journal.string()
              << ": run this from the bot's folder (cd /root/sniper)\n";
    return 1;
  }
  // The window ends at the journal's last row: the report reads the same
  // whenever it is run.
  double last_wall = 0;
  {
    std::ifstream f(journal, std::ios::binary);
    auto size = fs::file_size(journal);
    f.seekg(static_cast<std::streamoff>(size > 65536 ? size - 65536 : 0));
    std::string line;
    while (std::getline(f, line)) {
      auto r = Json::parse(line, nullptr, false);
      if (r.is_object())
        last_wall = std::max(last_wall, num(at(r, "wall")));
    }
  }
  const double since = last_wall - hours * 3600;

  long long bad = 0, rows = 0;
  std::vector<Run> runs;
  std::vector<std::pair<Json, Json>> run_counters; // per run: before the window, last
  struct Guard {
    double wall;
    std::string why;
    double until;
  };
  std::vector<Guard> guards;
  struct Held {
    std::string detail;
    double first, last;
    long long n;
  };
  std::vector<Held> held_periods;
  Tally refused_why, venue_refusals, rest_out, rest_skip, rest_done, rest_cancel_why, ws_down,
      ws_reason, ws_up, discovery;
  std::vector<std::pair<std::string, Error>> errors;
  long long fak_sent = 0, fak_miss = 0, fak_unknown = 0, rest_max_try = 0, coinbase_reconnects = 0,
            manual = 0, not_found = 0;
  std::vector<double> net_ms, feed_outage_left;
  std::map<std::string, long long> fills, fill_sh, fill_cost, fill_fee;
  std::vector<std::pair<double, std::string>> rest_off;
  std::vector<std::tuple<double, bool, std::string>> rest_on_rows;
  std::vector<Json> ptb_rows, settlements;
  std::vector<std::pair<double, std::string>> drawdown;
  // SNIPER_EXTRA_FEEDS: the trigger feed of each order, and what each feed did
  std::map<std::string, std::string> order_feed;
  std::map<std::string, long long> feed_sent, feed_miss, feed_fills;
  std::map<std::string, double> feed_sh;

  auto note_error = [&](const std::string &reason, double wall, const Json &r) {
    auto key = shorten(reason, 100);
    auto it = std::find_if(errors.begin(), errors.end(), [&](auto &e) { return e.first == key; });
    if (it == errors.end()) {
      errors.push_back({key, Error{0, wall, wall, ""}});
      it = std::prev(errors.end());
    }
    ++it->second.n;
    it->second.last = wall;
    if (it->second.extra.empty() && !str(at(r, "trade_id")).empty())
      it->second.extra = "trade " + shorten(str(at(r, "trade_id")), 24);
  };
  auto feed_of = [&](const std::string &id) {
    auto f = order_feed.find(id);
    return f == order_feed.end() ? std::string("coinbase") : f->second;
  };

  std::ifstream in(journal, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    // Most rows are routine skips whose totals the summaries carry: only
    // those that say why orders were held are read.
    if (line.find("\"result\":\"skipped\"") != std::string::npos &&
        line.find("\"reason\":\"live ") == std::string::npos)
      continue;
    auto r = Json::parse(line, nullptr, false);
    if (!r.is_object()) {
      ++bad;
      continue;
    }
    const double wall = num(at(r, "wall"));
    const std::string res = str(at(r, "result"));
    if (res == "start" && at(r, "native_cpp") == Json(true)) {
      runs.push_back(Run{wall, std::nullopt, false, Json()});
      run_counters.emplace_back(Json::object(), Json::object());
    }
    if (res == "summary" && !run_counters.empty()) {
      const Json &c = at(r, "counters").is_object() ? at(r, "counters") : Json::object();
      if (wall < since)
        run_counters.back().first = c;
      run_counters.back().second = c;
      if (!runs.empty()) {
        runs.back().end = wall;
        if (at(r, "final") == Json(true)) {
          runs.back().final = true;
          runs.back().exit_code = at(r, "exit_code");
        }
      }
    }
    if (wall < since)
      continue;
    ++rows;
    if (!runs.empty() && (!runs.back().end || wall > *runs.back().end))
      runs.back().end = wall;
    const std::string oid = str(at(r, "order_id"));
    if (res == "guard") {
      guards.push_back({wall, str(at(r, "reason")), num(at(r, "pause_until"), wall)});
    } else if (res == "skipped") {
      auto why = str(at(r, "reason"));
      if (why == "live halted" || why == "live refused") {
        auto detail = str(at(r, "detail"));
        if (why == "live refused")
          refused_why.add(shorten(detail, 80));
        else if (!held_periods.empty() && held_periods.back().detail == detail &&
                 wall - held_periods.back().last < 120) {
          held_periods.back().last = wall;
          ++held_periods.back().n;
        } else
          held_periods.push_back({detail, wall, wall, 1});
      }
    } else if (res == "error") {
      note_error(str(at(r, "reason")), wall, r);
    } else if (res == "pending" && !oid.empty() && at(r, "acknowledged") != Json(true)) {
      if (at(r, "never_retry") == Json(true)) {
        ++fak_unknown;
        note_error("unknown order outcome: " + str(at(r, "reason")), wall, r);
      } else if (r.contains("side")) {
        ++fak_sent;
        auto feed = str(at(r, "feed"));
        order_feed[oid] = feed.empty() ? "coinbase" : feed;
        ++feed_sent[order_feed[oid]];
      }
    } else if (res == "refused" && !oid.empty() && at(r, "acknowledged") == Json(true)) {
      if (str(at(r, "reason")) == "FAK no match") {
        ++fak_miss;
        ++feed_miss[feed_of(oid)];
      } else
        venue_refusals.add(shorten(str(at(r, "venue_reply")).empty() ? str(at(r, "reason"))
                                                                      : str(at(r, "venue_reply")),
                                   90));
      if (num(at(r, "T5_ns")) > 0 && num(at(r, "T6_ns")) > 0)
        net_ms.push_back((num(at(r, "T6_ns")) - num(at(r, "T5_ns"))) / 1e6);
    } else if (res == "live_fill_update" && at(r, "acknowledged") == Json(true)) {
      if (num(at(r, "T5_ns")) > 0 && num(at(r, "T6_ns")) > 0)
        net_ms.push_back((num(at(r, "T6_ns")) - num(at(r, "T5_ns"))) / 1e6);
    } else if (res == "live_fill") {
      std::string kind = at(r, "rest") == Json(true) ? "resting" : "FAK";
      ++fills[kind];
      fill_sh[kind] += std::llround(num(at(r, "shares")) * U);
      fill_cost[kind] += std::llround(num(at(r, "amount")) * U);
      fill_fee[kind] += std::llround(num(at(r, "fee")) * U);
      ++feed_fills[feed_of(oid)];
      feed_sh[feed_of(oid)] += num(at(r, "shares"));
    } else if (res == "rest_order") {
      auto out = str(at(r, "outcome"));
      rest_out.add(out.empty() ? "?" : out);
      if (out != "resting" && out != "matched" && out != "accepted")
        rest_skip.add(shorten(str(at(r, "reason")), 70));
      if (!oid.empty()) // a resting order belongs to the feed of the miss it follows
        order_feed[oid] = feed_of(str(at(r, "of_order_id")));
    } else if (res == "rest_done") {
      rest_done.add(at(r, "filled") == Json(true) ? "filled" : "no fill");
      auto why = str(at(r, "cancel_why"));
      rest_cancel_why.add(shorten(why.empty() ? "filled in full" : why, 50));
    } else if (res == "rest_cancel") {
      rest_max_try = std::max(rest_max_try, static_cast<long long>(num(at(r, "try"))));
    } else if (res == "rest_live_off") {
      rest_off.emplace_back(wall, str(at(r, "reason")));
    } else if (res == "rest_live") {
      rest_on_rows.emplace_back(wall, at(r, "on") == Json(true), str(at(r, "reason")));
    } else if (res == "ws") {
      auto socket = str(at(r, "socket"));
      if (at(r, "up") == Json(true))
        ws_up.add(socket);
      else {
        ws_down.add(socket + " " + str(at(r, "role")));
        ws_reason.add(socket + ": " + shorten(str(at(r, "reason")), 60));
        if (socket == "market" && str(at(r, "role")).find("no standby") != std::string::npos)
          feed_outage_left.push_back(num(at(r, "left_s"), 1e9));
      }
    } else if (res == "coinbase_reconnect" || res == "feed_error") {
      ++coinbase_reconnects;
    } else if (res == "discovery_refused") {
      discovery.add(shorten(str(at(r, "reason")), 70));
    } else if (res == "ptb_official") {
      ptb_rows.push_back(r);
    } else if (res == "settlement") {
      settlements.push_back(r);
    } else if (res == "hybrid_drawdown_pause" || res == "hybrid_drawdown_reset") {
      drawdown.emplace_back(wall, res);
    } else if (res == "manual_trade") {
      ++manual;
    } else if (res == "order_not_found") {
      ++not_found;
    }
  }

  const double now = static_cast<double>(std::time(nullptr));
  std::ostringstream out;
  auto p = [&](const std::string &s) { out << s << '\n'; };
  p("LIVE CHECK  " + state + "  window " + hm(since) + " .. " + hm(last_wall) + " UTC (" +
    sf("%g", hours) + " h)");
  const double age = now - last_wall;
  p(sf("journal: %lld rows in the window, %lld unreadable; last row ", rows, bad) + dur(age) +
    " ago" + (age > 180 ? "  -> the bot looks STOPPED" : "  (bot running)"));

  // ---- runs
  p("");
  p("RUNS");
  std::vector<Run> shown;
  for (auto &r : runs)
    if (r.end.value_or(r.start) >= since)
      shown.push_back(r);
  for (std::size_t i = shown.size() > 8 ? shown.size() - 8 : 0; i < shown.size(); ++i) {
    auto &r = shown[i];
    std::string st = r.final ? "stopped, exit " + str(r.exit_code)
                     : r.end && now - *r.end < 180
                         ? "running"
                         : "ended without a final summary (killed or crashed)";
    p("  " + hm(r.start) + " -> " + hm(r.end.value_or(0)) + sf("  %7s  ", dur(r.end.value_or(r.start) - r.start).c_str()) + st);
  }
  if (shown.empty())
    p("  no start in the window");

  // ---- counters in the window, summed over runs
  std::map<std::string, double> total;
  for (auto &[before, last] : run_counters)
    for (auto it = last.begin(); it != last.end(); ++it)
      if (it.value().is_number())
        total[it.key()] += it.value().get<double>() -
                           (before.contains(it.key()) && before[it.key()].is_number()
                                ? before[it.key()].get<double>()
                                : 0.0);
  p("");
  p("SIGNALS AND SKIPS (exact counters)");
  p(sf("  signals %.0f   orders decided %.0f", total["signals"], total["orders"]));
  std::vector<std::pair<std::string, double>> skips;
  for (auto &[k, v] : total)
    if (k.rfind("skip: ", 0) == 0 && v > 0)
      skips.emplace_back(k.substr(6), v);
  std::stable_sort(skips.begin(), skips.end(), [](auto &a, auto &b) { return a.second > b.second; });
  for (std::size_t i = 0; i < skips.size() && i < 10; ++i)
    p(sf("  %7.0f  ", skips[i].second) + shorten(skips[i].first, 70));

  // ---- trigger feeds (SNIPER_EXTRA_FEEDS)
  std::set<std::string> feeds;
  for (auto &[k, v] : total)
    if (k.rfind("signals ", 0) == 0 || k.rfind("fired ", 0) == 0)
      feeds.insert(k.substr(k.find(' ') + 1));
  for (auto &[f, n] : feed_sent)
    feeds.insert(f);
  if (feeds.size() > 1 || (feeds.size() == 1 && *feeds.begin() != "coinbase")) {
    feeds.insert("coinbase"); // the primary, even when it fired nothing
    p("");
    p("TRIGGER FEEDS (which feed fired each order)");
    for (auto &f : feeds) {
      double signals = f == "coinbase" ? total["signals"] : total["signals " + f];
      p(sf("  %-13s signals %7.0f  fired %5.0f  FAK sent %4lld  missed %4lld  fills %4lld  %8.1f sh",
           f.c_str(), signals, total["fired " + f], feed_sent[f], feed_miss[f], feed_fills[f],
           feed_sh[f]));
    }
  }

  // ---- halts
  p("");
  p("HALTS (new orders held)");
  if (!guards.empty()) {
    p("  fault pauses (5 min each unless repeated):");
    struct G {
      long long n = 0;
      double first = 0, last = 0, secs = 0;
    };
    std::vector<std::pair<std::string, G>> by;
    for (auto &g : guards) {
      auto key = shorten(g.why, 70);
      auto it = std::find_if(by.begin(), by.end(), [&](auto &e) { return e.first == key; });
      if (it == by.end()) {
        by.push_back({key, G{0, g.wall, g.wall, 0}});
        it = std::prev(by.end());
      }
      ++it->second.n;
      it->second.last = g.wall;
      it->second.secs += std::max(0.0, g.until - g.wall);
    }
    for (auto &[why, e] : by)
      p(sf("    %3lldx  ", e.n) + why + "  (" + hm(e.first) + " .. " + hm(e.last) + ", up to " +
        dur(e.secs) + " paused)");
  }
  if (!held_periods.empty()) {
    p("  held stretches ('live halted', from rate-limited rows):");
    struct M {
      long long n = 0;
      double secs = 0, first = 0, last = 0;
    };
    std::vector<std::pair<std::string, M>> merged;
    for (auto &h : held_periods) {
      auto key = shorten(h.detail, 70);
      auto it = std::find_if(merged.begin(), merged.end(), [&](auto &e) { return e.first == key; });
      if (it == merged.end()) {
        merged.push_back({key, M{0, 0, h.first, h.last}});
        it = std::prev(merged.end());
      }
      ++it->second.n;
      it->second.secs += std::max(1.0, h.last - h.first);
      it->second.last = h.last;
    }
    std::stable_sort(merged.begin(), merged.end(), [](auto &a, auto &b) { return a.second.secs > b.second.secs; });
    for (std::size_t i = 0; i < merged.size() && i < 8; ++i)
      p(sf("    %7s in %3lld stretches  ", dur(merged[i].second.secs).c_str(), merged[i].second.n) +
        merged[i].first + "  (last " + hm(merged[i].second.last) + ")");
  }
  if (!drawdown.empty()) {
    long long pauses = std::count_if(drawdown.begin(), drawdown.end(),
                                     [](auto &d) { return d.second == "hybrid_drawdown_pause"; });
    p(sf("  drawdown stop: %lld pauses, last ", pauses) + hm(drawdown.back().first));
  }
  if (!refused_why.empty()) {
    p("  refused before sending ('live refused'):");
    for (auto &[why, n] : refused_why.top(6))
      p(sf("    %5lld  ", n) + why);
  }
  if (guards.empty() && held_periods.empty() && drawdown.empty() && refused_why.empty())
    p("  none");

  // ---- errors
  p("");
  p("ERRORS");
  if (!errors.empty()) {
    auto sorted = errors;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) { return a.second.n > b.second.n; });
    for (std::size_t i = 0; i < sorted.size() && i < 10; ++i) {
      auto &[why, e] = sorted[i];
      p(sf("  %5lldx  ", e.n) + why + "  (" + hm(e.first) + " .. " + hm(e.last) + ")" +
        (e.extra.empty() ? "" : "  " + e.extra));
    }
  } else
    p("  none");
  if (manual)
    p(sf("  %lld trades listed in manual_trades.txt were skipped", manual));
  if (not_found)
    p(sf("  %lld orders the venue had no record of a minute after sending (taken as no fill)", not_found));

  // ---- orders and fills
  p("");
  p("ORDERS AND FILLS");
  p(sf("  FAK sent %lld   missed %lld   venue refused %lld   unknown outcome %lld", fak_sent,
       fak_miss, venue_refusals.total(), fak_unknown));
  for (const char *k : {"FAK", "resting"})
    if (fills[k])
      p(sf("  %-7s fills %4lld  %8.1f sh  cost %8.2f  fees %.2f", k, fills[k],
           static_cast<double>(fill_sh[k]) / U, static_cast<double>(fill_cost[k]) / U,
           static_cast<double>(fill_fee[k]) / U));
  if (fak_sent)
    p(sf("  FAK fill rate %.1f%% of orders sent", static_cast<double>(fills["FAK"]) / fak_sent * 100));
  for (auto &[why, n] : venue_refusals.top(4))
    p(sf("    refused %lldx: ", n) + why);
  if (!net_ms.empty())
    p(sf("  order round trip ms  p50 %.0f  p95 %.0f  max %.0f  (%zu replies)", *pct(net_ms, 50),
         *pct(net_ms, 95), *std::max_element(net_ms.begin(), net_ms.end()), net_ms.size()));

  // ---- resting
  if (!rest_out.empty() || !rest_on_rows.empty() || !rest_off.empty()) {
    p("");
    p("RESTING ORDERS");
    if (!rest_on_rows.empty()) {
      auto &[w, on, why] = rest_on_rows.back();
      p(std::string("  at last start: ") + (on ? "ON" : "OFF") + "  " + shorten(why, 80));
    }
    p("  after a miss: " + joined(rest_out.top()));
    for (auto &[why, n] : rest_skip.top(5))
      p(sf("    %4lld  not rested: ", n) + why);
    if (!rest_done.empty())
      p("  ended: " + joined(rest_done.items) + "   cancel reasons: " + joined(rest_cancel_why.top(4)));
    if (rest_max_try > 3)
      p(sf("  !! a cancel needed %lld tries", rest_max_try));
    for (auto &[w, why] : rest_off)
      p("  !! test switched OFF at " + hm(w) + ": " + why);
  }

  // ---- sockets
  p("");
  p("WEBSOCKETS");
  if (!ws_down.empty() || !ws_up.empty()) {
    p("  connected: " + joined(ws_up.items, "x"));
    p("  dropped:   " + (ws_down.empty() ? std::string("none") : joined(ws_down.top(), "x")));
    for (auto &[why, n] : ws_reason.top(5))
      p(sf("    %4lldx  ", n) + why);
    if (!feed_outage_left.empty()) {
      long long near_end = std::count_if(feed_outage_left.begin(), feed_outage_left.end(),
                                         [](double x) { return x <= 15; });
      p(sf("  !! market feed fully down %zux (%lld in a round's last 15 s)", feed_outage_left.size(), near_end));
    }
  } else
    p("  no socket rows (an older build, or nothing happened)");
  if (coinbase_reconnects)
    p(sf("  trigger feed reconnects asked by the bot: %lld", coinbase_reconnects));
  for (auto &[why, n] : discovery.top(3))
    p(sf("  discovery refused %lldx: ", n) + why);

  // ---- price to beat
  p("");
  std::vector<double> errs;
  for (auto &r : ptb_rows)
    if (at(r, "err_bp").is_number())
      errs.push_back(at(r, "err_bp").get<double>());
  std::string ptb = sf("PRICE TO BEAT: %zu published values read, checks now ", ptb_rows.size()) +
                    (ptb_rows.empty() ? std::string("None") : str(at(ptb_rows.back(), "checks")));
  if (!errs.empty())
    ptb += sf(", estimate error p50 %.2f bp max %.2f bp", *pct(errs, 50),
              *std::max_element(errs.begin(), errs.end()));
  p(ptb);

  // ---- ledger
  p("");
  p("LEDGER");
  Json led;
  try {
    std::ifstream f(ledger_path);
    f >> led;
  } catch (const std::exception &e) {
    p(std::string("  ledger unreadable: ") + e.what());
    led = Json();
  }
  if (led.is_object()) {
    const Json &orders = at(led, "orders").is_array() ? led["orders"] : Json::array();
    std::map<std::string, Json> pay;
    for (auto &s : settlements)
      pay[str(at(s, "condition_id"))] = at(s, "payouts").is_object() ? s["payouts"] : Json::object();
    long long pnl_fak = 0, pnl_rest = 0, wins = 0, losses = 0;
    std::map<std::string, long long> feed_pnl;
    for (auto &[cond, payouts] : pay) {
      long long round_v = 0;
      bool any = false;
      for (auto &o : orders) {
        if (str(at(o, "condition")) != cond || num(at(o, "booked_shares")) <= 0)
          continue;
        any = true;
        long long paid = payouts.contains(str(at(o, "token")))
                             ? static_cast<long long>(num(payouts[str(at(o, "token"))]))
                             : 0;
        long long v = static_cast<long long>(num(at(o, "booked_shares"))) * paid / U -
                      static_cast<long long>(num(at(o, "booked_cost"))) -
                      static_cast<long long>(num(at(o, "booked_fee")));
        (at(o, "rest") == Json(true) ? pnl_rest : pnl_fak) += v;
        feed_pnl[feed_of(str(at(o, "id")))] += v;
        round_v += v;
      }
      if (any) {
        wins += round_v > 0;
        losses += round_v < 0;
      }
    }
    p(sf("  settled in the window: %zu rounds, %lld won / %lld lost, P&L FAK ", pay.size(), wins, losses) +
      money(pnl_fak) + "  resting " + money(pnl_rest) + "  total " + money(pnl_fak + pnl_rest));
    if (feed_pnl.size() > 1 || (feed_pnl.size() == 1 && feed_pnl.begin()->first != "coinbase")) {
      std::string line = "  settled P&L by trigger feed:";
      for (auto &[f, v] : feed_pnl)
        line += "  " + f + " " + money(v);
      p(line);
    }
    p("  all time: realized " + money(static_cast<long long>(num(at(led, "realized")))) +
      "  of it resting " + money(static_cast<long long>(num(at(led, "rest_realized")))) +
      sf("  fees $%.2f", num(at(led, "fees")) / U));
    long long open_n = 0;
    double open_sh = 0, open_cost = 0;
    if (at(led, "positions").is_array())
      for (auto &x : led["positions"])
        if (at(x, "settled") != Json(true) && num(at(x, "shares")) > 0) {
          ++open_n;
          open_sh += num(at(x, "shares")) / U;
          open_cost += (num(at(x, "cost")) + num(at(x, "fees"))) / U;
        }
    if (open_n)
      p(sf("  open positions: %lld  %.1f sh  cost %.2f", open_n, open_sh, open_cost));
    int listed = 0;
    std::set<std::string> settled;
    if (at(led, "settled_conditions").is_array())
      for (auto &c : led["settled_conditions"])
        settled.insert(str(c));
    std::set<std::string> late;
    for (auto &o : orders) {
      auto st = str(at(o, "status"));
      if ((st == "intent" || st == "sent" || st == "unknown" || st == "pending" || st == "matched") &&
          listed++ < 6)
        p("  !! order " + str(at(o, "id")).substr(0, 10) + " " + str(at(o, "side")) + " " +
          (at(o, "rest") == Json(true) ? "resting " : "") + "status " + st + " for " +
          dur(now - num(at(o, "sent_wall"), now)));
      if (num(at(o, "booked_shares")) > 0 && !settled.count(str(at(o, "condition"))) &&
          num(at(o, "end")) < now - 1800)
        late.insert(str(at(o, "condition")));
    }
    if (!late.empty())
      p(sf("  !! %zu rounds with shares still unsettled 30 min after they ended", late.size()));
    const Json &c = at(led, "canary");
    const bool paused = num(at(c, "pause_until")) > now;
    p("  canary session since " + hm(num(at(c, "started"))) +
      sf(": orders %.0f  fills %.0f  spend $%.2f  rounds %zu  settled loss $%.2f", num(at(c, "orders")),
         num(at(c, "fills")), num(at(c, "spend")) / U,
         at(c, "rounds").is_array() ? c["rounds"].size() : std::size_t(0), num(at(c, "settled_loss")) / U) +
      (paused ? "  PAUSED " + dur(num(at(c, "pause_until")) - now) + " more: " + str(at(c, "pause_reason"))
              : ""));
  }
  const auto test = fs::path(state) / "rest_live_test.json";
  if (fs::exists(test)) {
    Json t = Json::parse(std::ifstream(test), nullptr, false);
    if (t.is_object())
      p(std::string("  resting test: ") +
        (at(t, "off") == Json(true) ? "OFF (" + str(at(t, "reason")) + ")" : "on") +
        ", result since it began " + money(static_cast<long long>(num(at(t, "realized")))));
    else
      p("  resting test record unreadable");
  }
  if (fs::exists("STOP_LIVE") || fs::exists(fs::path(state).parent_path().parent_path() / "STOP_LIVE"))
    p("  !! STOP_LIVE exists: new orders are blocked");
  std::cout << out.str();
  return 0;
}
