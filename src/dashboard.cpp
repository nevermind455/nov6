// Port of dashboard/safety.py, widgets.py, theme.py and sniper_layout.py
// (nevermind455/oct10@772f2eb). Pure text and layout: the terminal output and
// its thread are in dashboard_terminal.cpp.
#include "sniper/dashboard.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <regex>
#include <unordered_map>

namespace sniper::dash {
namespace {
const std::string MISSING = "--";
const std::string PENDING_S = "PENDING SETTLEMENT";
// Box and block glyphs, as UTF-8.
const char *const G_TL = "\xe2\x94\x8c";
const char *const G_TR = "\xe2\x94\x90";
const char *const G_BL = "\xe2\x94\x94";
const char *const G_BR = "\xe2\x94\x98";
const char *const G_H = "\xe2\x94\x80";
const char *const G_V = "\xe2\x94\x82";
const char *const G_BLOCK = "\xe2\x96\x88";
const char *const G_LIGHT = "\xe2\x96\x91";
const char *const G_ELLIPSIS = "\xe2\x80\xa6";
const char *const G_DOT = "\xc2\xb7";
// Eighth-width blocks, thinnest first: a gauge that moves in whole cells jumps
// in 5-10% steps at these panel widths and reads as a stuck bar.
const std::array<const char *, 7> EIGHTHS = {
    "\xe2\x96\x8f", "\xe2\x96\x8e", "\xe2\x96\x8d", "\xe2\x96\x8c",
    "\xe2\x96\x8b", "\xe2\x96\x8a", "\xe2\x96\x89"};

constexpr Style S(Color fg, bool bold = false) {
  return Style{fg, Color::cream, bold};
}
constexpr Style PAPER{};
constexpr Style DIM = S(Color::dim);
constexpr Style FAINT = S(Color::faint);
constexpr Style RULE = S(Color::rule);
constexpr Style INK = S(Color::ink);
constexpr Style INK_B = S(Color::ink, true);
constexpr Style GREEN_B = S(Color::green, true);
constexpr Style RED_B = S(Color::red, true);
constexpr Style AMBER = S(Color::amber);
constexpr Style AMBER_B = S(Color::amber, true);
constexpr Style BLUE_B = S(Color::blue, true);

template <class... A> std::string sf(const char *format, A... a) {
  char buffer[192];
  int n = std::snprintf(buffer, sizeof buffer, format, a...);
  if (n < 0)
    return {};
  return std::string(buffer,
                     std::min<std::size_t>(static_cast<std::size_t>(n),
                                           sizeof buffer - 1));
}
std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return s;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}
// Up to `n` whole characters of a UTF-8 string.
std::string take(std::string_view s, std::size_t n) {
  std::size_t seen = 0, i = 0;
  for (; i < s.size(); ++i)
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
      if (seen == n)
        break;
      ++seen;
    }
  return std::string(s.substr(0, i));
}
std::string repeat(const char *glyph, int n) {
  std::string out;
  for (int i = 0; i < n; ++i)
    out += glyph;
  return out;
}

// ---------------------------------------------------------- values
const Json &get(const Json &o, const char *key) {
  static const Json null;
  if (!o.is_object())
    return null;
  auto it = o.find(key);
  return it == o.end() ? null : *it;
}
// A finite number, from a number or a numeric string; never a boolean.
std::optional<double> num(const Json &v) {
  double f;
  if (v.is_number())
    f = v.get<double>();
  else if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    char *end = nullptr;
    f = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size())
      return std::nullopt;
  } else
    return std::nullopt;
  if (!std::isfinite(f))
    return std::nullopt;
  return f;
}
Json opt(std::optional<double> v) { return v ? Json(*v) : Json(); }
bool truthy(const Json &v) {
  if (v.is_null())
    return false;
  if (v.is_boolean())
    return v.get<bool>();
  if (v.is_number())
    return v.get<double>() != 0;
  if (v.is_string() || v.is_array() || v.is_object())
    return !v.empty();
  return true;
}
std::string text(const Json &v, const std::string &fallback = MISSING) {
  if (v.is_null())
    return fallback;
  if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    return s.empty() ? fallback : terminal_text(s, 400);
  }
  return terminal_text(v.dump(-1, ' ', false, Json::error_handler_t::replace),
                       400);
}
// {:,.Nf}
std::string commas(double v, int decimals) {
  auto s = sf("%.*f", decimals, v);
  auto dot = s.find('.');
  int end = static_cast<int>(dot == std::string::npos ? s.size() : dot);
  int first = !s.empty() && s[0] == '-' ? 1 : 0;
  for (int i = end - 3; i > first; i -= 3)
    s.insert(static_cast<std::size_t>(i), ",");
  return s;
}
std::string money(const Json &v, bool sign = false) {
  auto f = num(v);
  if (!f)
    return MISSING;
  std::string prefix = sign && *f > 0 ? "+" : *f < 0 ? "-" : "";
  return prefix + "$" + commas(std::abs(*f), 2);
}
std::string price(const Json &v) {
  auto f = num(v);
  return f ? sf("%.3f", *f) : MISSING;
}
std::string bp(const Json &v) {
  auto f = num(v);
  return f ? sf("%+.2fbp", *f) : MISSING;
}
std::string count(const Json &v) {
  auto f = num(v);
  return f ? std::to_string(static_cast<long long>(*f)) : MISSING;
}
std::string ratio(const Json &a, const Json &b) {
  return count(a) + "/" + count(b);
}
// A share price as the position table shows it: .460, 1.000, --.
std::string px(const Json &v) {
  auto f = num(v);
  if (!f)
    return MISSING;
  auto s = sf("%.3f", *f);
  return *f >= 0 && *f < 1 ? s.substr(1) : s;
}
// A latency in ms: 0.41ms, 12.3ms, 190ms; N/A without a sample.
std::string lat(const Json &v) {
  auto f = num(v);
  if (!f)
    return "N/A";
  return *f < 10 ? sf("%.2fms", *f) : *f < 100 ? sf("%.1fms", *f)
                                                : sf("%.0fms", *f);
}
std::string secs(const Json &v) {
  auto f = num(v);
  if (!f)
    return MISSING;
  return *f < 100 ? sf("%.1fs", *f) : sf("%.0fs", *f);
}
std::string strip_ms(std::string s) {
  auto at = s.find("ms");
  if (at != std::string::npos)
    s.erase(at, 2);
  return s;
}
std::vector<std::string> wrap(const std::string &in, int width_) {
  std::vector<std::string> out;
  std::size_t w = static_cast<std::size_t>(std::max(1, width_));
  std::string line;
  std::size_t i = 0;
  while (i < in.size()) {
    while (i < in.size() && in[i] == ' ')
      ++i;
    auto j = in.find(' ', i);
    std::string word =
        in.substr(i, j == std::string::npos ? std::string::npos : j - i);
    i = j == std::string::npos ? in.size() : j;
    if (word.empty())
      continue;
    while (word.size() > w) {
      if (!line.empty()) {
        out.push_back(line);
        line.clear();
      }
      out.push_back(word.substr(0, w));
      word.erase(0, w);
    }
    if (!line.empty() && line.size() + 1 + word.size() > w) {
      out.push_back(line);
      line.clear();
    }
    line += (line.empty() ? "" : " ") + word;
  }
  if (!line.empty())
    out.push_back(line);
  return out;
}

// ---------------------------------------------------------- styles
bool one_of(const std::string &s, std::initializer_list<const char *> set) {
  return std::any_of(set.begin(), set.end(),
                     [&](const char *x) { return s == x; });
}
bool is_good(const std::string &s) {
  return one_of(s, {"PASS", "OK", "UP", "LIVE", "FILLED", "CONNECTED",
                    "HEALTHY", "ON", "TRUE"});
}
bool is_bad(const std::string &s) {
  return one_of(s, {"BLOCK", "FAIL", "DOWN", "ERROR", "REJECTED",
                    "DISCONNECTED", "LOSS", "STALLED"});
}
bool is_wait(const std::string &s) {
  return one_of(s, {"WAIT", "PENDING", "STALE", "ARMED", "WARN", "IDLE",
                    "DEGRADED"});
}
bool is_absent(const std::string &s) {
  return one_of(s, {"ABSENT", "N/A", "--", ""});
}
// A filled block of colour, for the header strip.
Style state_style(const std::string &state) {
  auto s = upper(state);
  if (is_good(s))
    return Style{Color::white, Color::green, true};
  if (is_bad(s))
    return Style{Color::white, Color::red, true};
  if (is_wait(s))
    return Style{Color::ink, Color::amber, true};
  if (is_absent(s))
    return Style{Color::faint, Color::paper2, false};
  return Style{Color::ink, Color::paper2, false};
}
// The same meaning as coloured text on paper, for inside a panel.
Style state_fg(const std::string &state) {
  auto s = upper(state);
  if (is_good(s))
    return GREEN_B;
  if (is_bad(s))
    return RED_B;
  if (is_wait(s))
    return AMBER_B;
  if (is_absent(s))
    return FAINT;
  return INK;
}
Style pnl_style(std::optional<double> v, bool big = false) {
  if (!v)
    return FAINT;
  if (*v > 0)
    return S(big ? Color::green2 : Color::green, true);
  if (*v < 0)
    return S(big ? Color::red2 : Color::red, true);
  return S(Color::dim, big);
}
Style market_state_fg(const std::string &s) {
  if (s == "ACTIVE")
    return GREEN_B;
  if (s == "EXPIRED" || s == PENDING_S)
    return AMBER_B;
  if (s == "SETTLING")
    return BLUE_B;
  if (s == "SETTLED")
    return INK_B;
  if (s == "DISCOVERING")
    return FAINT;
  return INK;
}
Style market_state_badge(const std::string &s) {
  if (s == "ACTIVE")
    return Style{Color::white, Color::green, true};
  if (s == "EXPIRED" || s == PENDING_S)
    return Style{Color::ink, Color::amber, true};
  if (s == "SETTLING")
    return Style{Color::white, Color::blue, true};
  if (s == "SETTLED")
    return Style{Color::white, Color::rule, true};
  return Style{Color::ink, Color::paper2, true};
}
Style result_style(const std::string &r) {
  if (r == "filled")
    return GREEN_B;
  if (r == "missed" || r == "refused")
    return AMBER;
  if (r == "pending")
    return BLUE_B;
  if (r == "error" || r == "rejected")
    return RED_B;
  if (r == "unknown")
    return S(Color::purple);
  return INK;
}
Style level_style(const std::string &l) {
  if (l == "good")
    return S(Color::green);
  if (l == "warn")
    return AMBER;
  if (l == "bad")
    return RED_B;
  return INK;
}
// The NET line: green up to, amber up to, then red. Coinbase is the trigger
// feed's delay; the CLOB is a round trip.
Style net_style(const std::string &key, std::optional<double> ms) {
  if (!ms)
    return FAINT;
  double green = key == "coinbase" ? 80 : 40, amber = key == "coinbase" ? 150 : 80;
  return S(*ms <= green ? Color::green : *ms <= amber ? Color::amber : Color::red,
           true);
}

// ---------------------------------------------------------- rows
Row runs(std::vector<Run> parts, int cols) {
  parts.erase(std::remove_if(parts.begin(), parts.end(),
                             [](const Run &r) { return r.text.empty(); }),
              parts.end());
  return pad(std::move(parts), cols);
}
Row line(const std::string &label, const std::string &value, int cols,
         Style vstyle = PAPER, int lw = 9) {
  lw = std::max(1, std::min(lw, cols / 2));
  return pad({{fit(label, lw), DIM},
              {" ", PAPER},
              {fit(value, std::max(0, cols - lw - 1)), vstyle}},
             cols);
}
void add(std::vector<Row> &to, std::vector<Row> rows) {
  to.insert(to.end(), std::make_move_iterator(rows.begin()),
            std::make_move_iterator(rows.end()));
}
std::vector<Row> first(std::vector<Row> rows, int n) {
  if (static_cast<int>(rows.size()) > n)
    rows.resize(static_cast<std::size_t>(std::max(0, n)));
  return rows;
}

// ---------------------------------------------------------- panels
Row net_row(const Json &net, int cols) {
  auto make = [&](const char *coinbase, const char *clob) {
    std::vector<Run> out{{fit("NET ms", 9), DIM}, {" ", PAPER}};
    for (auto [key, name] : {std::pair<const char *, const char *>{"coinbase", coinbase},
                             std::pair<const char *, const char *>{"clob", clob}}) {
      auto v = num(get(net, key));
      out.push_back({std::string(name) + " ", FAINT});
      out.push_back({v ? sf("%.0f", *v) : MISSING, net_style(key, v)});
      out.push_back({"  ", PAPER});
    }
    return out;
  };
  auto full = make("coinbase", "clob");
  std::size_t used = 0;
  for (auto &r : full)
    used += cells(r.text);
  if (used - 2 > static_cast<std::size_t>(std::max(0, cols)))
    full = make("cb", "clob");
  return runs(std::move(full), cols);
}

Row header(const Json &snap, int cols) {
  auto mode = upper(text(get(snap, "mode")));
  const Json &canary = get(snap, "canary"), &market = get(snap, "market");
  auto left = num(get(market, "seconds_left"));
  std::vector<Run> parts{
      {" BTC SNIPER ", Style{Color::white, Color::rule, true}},
      {" ", PAPER},
      {" " + mode + " ", Style{Color::white, mode == "LIVE" ? Color::red : Color::green, true}}};
  if (truthy(canary)) {
    bool halted = truthy(get(canary, "halted"));
    parts.push_back({" ", PAPER});
    parts.push_back({halted ? " HALTED " : " CANARY ARMED ",
                     state_style(halted ? "FAIL" : "WARN")});
  } else if (mode == "PAPER" && cols >= 140) {
    parts.push_back({" ", PAPER});
    parts.push_back({std::string(" NO REAL ORDERS ") + G_DOT + " $0 AT RISK ",
                     Style{Color::ink, Color::paper2, true}});
  }
  parts.push_back({"  ", PAPER});
  parts.push_back({text(get(market, "slug"), "no round yet").substr(0, 40), INK_B});
  parts.push_back({"  ", PAPER});
  parts.push_back({left ? sf("T-%.0fs", *left) : "T-?", BLUE_B});
  const Json &st = get(snap, "settlement");
  auto state = text(get(get(st, "active"), "state"), "DISCOVERING");
  parts.push_back({"  ", PAPER});
  parts.push_back({" " + state + " ", market_state_badge(state)});
  auto waiting = num(get(st, "pending_count"));
  if (waiting && *waiting) {
    parts.push_back({" ", PAPER});
    parts.push_back({" " + std::to_string(static_cast<long long>(*waiting)) +
                         " PENDING SETTLEMENT ",
                     market_state_badge(PENDING_S)});
  }
  auto clock = text(get(snap, "clock"), "");
  std::size_t used = 0;
  for (auto &r : parts)
    used += cells(r.text);
  int gap = cols - static_cast<int>(used) - static_cast<int>(clock.size()) - 1;
  if (gap > 0) {
    parts.push_back({std::string(static_cast<std::size_t>(gap), ' '), PAPER});
    parts.push_back({clock, DIM});
    parts.push_back({" ", PAPER});
  }
  return pad(std::move(parts), cols);
}

std::vector<Row> trigger_body(const Json &t, int w) {
  auto status = text(get(t, "status"));
  auto move = num(get(t, "move_bp")), k = num(get(t, "k_bp"));
  bool hot = move && k && std::abs(*move) >= *k;
  auto age = num(get(t, "last_signal_age_s"));
  auto rate = num(get(t, "signals_per_min"));
  auto fr = num(get(t, "fill_rate"));
  auto window = num(get(t, "window_ms"));
  auto mid = num(get(t, "mid"));
  const Json &busy = get(t, "in_flight");
  std::vector<Row> rows{
      runs({{fit("feed", 9), DIM},
            {" ", PAPER},
            {text(get(t, "feed")) + " ", INK_B},
            {status, state_fg(status)},
            {busy.is_string() ? "  busy " + text(busy) : truthy(busy) ? "  busy" : "",
             S(Color::blue)}},
           w),
      runs({{fit("BTC mid", 9), DIM},
            {" ", PAPER},
            {mid ? commas(*mid, 2) : MISSING, INK_B}},
           w)};
  const Json &fair = get(t, "fair");
  if (fair.is_object()) {
    auto vol = num(get(fair, "vol_bp"));
    const Json &last = get(fair, "last");
    auto c = [](const Json &x) {
      auto f = num(x);
      if (!f)
        return MISSING;
      auto s = sf("%.2f", *f);
      return s.rfind("0", 0) == 0 ? s.substr(1) : s;
    };
    if (!vol) {
      rows.push_back(runs({{fit("fair", 9), DIM},
                           {" ", PAPER},
                           {"unknown: " + text(get(fair, "why")) + " - no buys", AMBER}},
                          w));
    } else {
      std::vector<Run> parts{{fit("fair", 9), DIM},
                             {" ", PAPER},
                             {sf("vol %.2fbp", *vol), FAINT}};
      if (last.is_object()) {
        auto side = text(get(last, "side"));
        auto sh = num(get(last, "shares"));
        parts.push_back({"  ", PAPER});
        parts.push_back({side, side == "UP" ? GREEN_B : RED_B});
        parts.push_back({" " + c(get(last, "market_mid")) + "->" + c(get(last, "fair")), INK_B});
        parts.push_back({" <=" + c(get(last, "limit")), DIM});
        parts.push_back({sh && *sh ? sf(" %gsh", *sh) : "", INK});
      }
      rows.push_back(runs(std::move(parts), w));
    }
  }
  bool has_signal = !get(t, "last_signal_bp").is_null();
  rows.push_back(runs({{fit("move", 9), DIM},
                       {" ", PAPER},
                       {bp(opt(move)), hot ? RED_B : INK},
                       {window ? sf(" /%.0fms", *window) : "", DIM},
                       {k ? sf(" k%g", *k) : "", FAINT}},
                      w));
  rows.push_back(runs({{fit("signals", 9), DIM},
                       {" ", PAPER},
                       {count(get(t, "signals")), INK_B},
                       {rate ? sf("  %.1f/min", *rate) : "", DIM},
                       {has_signal ? "  last " : "", DIM},
                       {has_signal ? bp(get(t, "last_signal_bp")) : "", INK},
                       {age && has_signal ? sf(" %.0fs ago", *age) : "", DIM}},
                      w));
  rows.push_back(runs({{fit("orders", 9), DIM},
                       {" ", PAPER},
                       {count(get(t, "orders")), INK_B},
                       {"  fill ", DIM},
                       {count(get(t, "filled")), GREEN_B},
                       {"  miss ", DIM},
                       {count(get(t, "refused")), AMBER},
                       {fr ? sf("  %.0f%%", *fr * 100) : "", DIM}},
                      w));
  auto slip = num(get(t, "slip")), errors = num(get(t, "errors"));
  rows.push_back(runs({{fit("order", 9), DIM},
                       {" ", PAPER},
                       {text(get(t, "order_type")), INK},
                       {slip ? sf(" +%.2f", *slip) : "", DIM},
                       {" snd " + lat(get(t, "send_p50_ms")), INK},
                       {" ack " + lat(get(t, "ack_p50_ms")) +
                            (truthy(get(t, "ack_simulated")) ? " sim" : ""),
                        DIM},
                       {errors && *errors ? "  err " + count(get(t, "errors")) : "", RED_B}},
                      w));
  return rows;
}

std::vector<Row> market_body(const Json &m, int w) {
  auto left = num(get(m, "seconds_left")), until = num(get(m, "trade_until_s"));
  int bar_w = std::max(3, w - 18);
  Style bar_style = left && until && *left < *until ? AMBER : S(Color::blue);
  std::vector<Run> left_parts{{fit("left", 9), DIM}, {" ", PAPER}};
  for (auto &r : bar(left.value_or(0.0) / 300.0, bar_w, bar_style))
    left_parts.push_back(r);
  left_parts.push_back({left ? sf(" %4.0fs", *left) : "    --", INK_B});
  int cw = std::max(4, (w - 10) / 2);
  auto side = [&](const char *name) {
    const Json &s = get(m, name);
    auto age = num(get(s, "age_ms"));
    return std::array<std::string, 4>{price(get(s, "bid")), price(get(s, "ask")),
                                      text(get(s, "status")),
                                      age ? sf("%.0fms", *age) : MISSING};
  };
  auto up = side("up"), dn = side("down");
  auto pair = [&](const std::string &label, const std::string &a,
                  const std::string &b, Style sa = INK, Style sb = INK) {
    return runs({{fit(label, 9), DIM}, {" ", PAPER}, {fit(a, cw), sa}, {fit(b, cw), sb}}, w);
  };
  auto up_sh = num(get(m, "round_up_sh")), dn_sh = num(get(m, "round_down_sh"));
  auto max_sh = num(get(m, "max_side_shares"));
  return {line("round", text(get(m, "slug"), "waiting for discovery"), w, INK_B),
          runs(std::move(left_parts), w),
          runs({{std::string(10, ' '), PAPER},
                {fit("UP", cw), GREEN_B},
                {fit("DOWN", cw), RED_B}},
               w),
          pair("best bid", up[0], dn[0]),
          pair("best ask", up[1], dn[1], INK_B, INK_B),
          pair("book", up[2] + " " + up[3], dn[2] + " " + dn[3], state_fg(up[2]),
               state_fg(dn[2])),
          runs({{fit("this rnd", 9), DIM},
                {" ", PAPER},
                {"fills ", DIM},
                {ratio(get(m, "round_fills"), get(m, "max_fills")), INK},
                {"  UP ", DIM},
                {up_sh ? sf("%.0f", *up_sh) : MISSING, INK},
                {"  DN ", DIM},
                {dn_sh ? sf("%.0f", *dn_sh) : MISSING, INK},
                {max_sh ? sf(" /%.0fsh", *max_sh) : "", FAINT}},
               w)};
}

std::vector<Row> account_body(const Json &a, int w) {
  auto kind = upper(text(get(a, "kind"), ""));
  bool full = w >= 30;
  int lw = full ? 14 : 9;
  auto realized = num(get(a, "realized")), unreal = num(get(a, "unrealized"));
  auto total = !get(a, "total").is_null()
                   ? num(get(a, "total"))
                   : (kind != "LIVE" ? num(get(a, "pnl")) : std::nullopt);
  auto row = [&](const char *long_label, const char *short_label,
                 const std::string &value, Style style = INK) {
    return line(full ? long_label : short_label, value, w, style, lw);
  };
  std::vector<Row> rows;
  if (kind == "LIVE") {
    rows.push_back(row("COLLATERAL", "collatrl", money(get(a, "balance")), INK_B));
    auto redeem = num(get(a, "redeemable"));
    rows.push_back(row("REDEEMABLE", "redeemabl", money(opt(redeem)),
                       redeem && *redeem ? AMBER_B : INK));
  } else
    rows.push_back(row("CASH", "cash", money(get(a, "cash")), INK_B));
  rows.push_back(row("POSITION VALUE", "pos value", money(get(a, "position_value"))));
  rows.push_back(row("EQUITY", "equity", money(get(a, "equity")), INK_B));
  rows.push_back(row("REALIZED P/L", "realized", money(opt(realized), true), pnl_style(realized)));
  rows.push_back(row("UNREALIZED P/L", "unrealzd", money(opt(unreal), true), pnl_style(unreal)));
  rows.push_back(row("TOTAL P/L", "total P/L", money(opt(total), true), pnl_style(total, true)));
  if (kind == "LIVE") {
    auto status = text(get(a, "balance_status"), "");
    rows.push_back(row("BALANCE CHECK", "bal check", text(get(a, "balance_status")),
                       state_fg(status)));
  } else
    rows.push_back(row("FEES  WON/LOST", "fees",
                       money(get(a, "fees")) + "  " + count(get(a, "wins")) + "/" +
                           count(get(a, "losses"))));
  return rows;
}

std::vector<Row> orders_body(const Json &orders, int w, int h) {
  const std::array<int, 7> widths{8, 4, 7, 5, 5, 7, 13};
  const std::array<const char *, 7> head{"TIME", "SIDE", "BTC", "ASK", "LIMIT", "RESULT", "FILLED"};
  int used = 7;
  for (int n : widths)
    used += n;
  int detail_w = std::max(0, w - used);
  std::vector<Run> head_row;
  for (std::size_t i = 0; i < widths.size(); ++i)
    head_row.push_back({fit(head[i], widths[i]) + " ", S(Color::dim, true)});
  if (detail_w)
    head_row.push_back({fit("DETAIL", detail_w), S(Color::dim, true)});
  std::vector<Row> out{runs(std::move(head_row), w)};
  if (!orders.is_array() || orders.empty()) {
    out.push_back(runs({{"  no orders yet in this market", FAINT}}, w));
    return out;
  }
  int shown = std::min(static_cast<int>(orders.size()), std::max(0, h - 1));
  for (int i = 0; i < shown; ++i) { // newest first
    const Json &o = orders[orders.size() - 1 - static_cast<std::size_t>(i)];
    auto side = text(get(o, "side"));
    auto result = lower(text(get(o, "result")));
    auto sh = num(get(o, "shares")), p = num(get(o, "price"));
    auto btc = bp(get(o, "bp"));
    auto at = btc.find("bp");
    if (at != std::string::npos)
      btc.erase(at);
    std::array<Run, 7> row_cells{
        Run{text(get(o, "time")), DIM},
        Run{side, side == "UP" ? GREEN_B : RED_B},
        Run{btc, INK},
        Run{price(get(o, "ask")), INK},
        Run{price(get(o, "cap")), DIM},
        Run{upper(result), result_style(result)},
        Run{result == "filled" && sh && p ? sf("%.1f@%.3f", *sh, *p) : "", INK}};
    std::vector<Run> row;
    for (std::size_t c = 0; c < row_cells.size(); ++c)
      row.push_back({fit(row_cells[c].text, widths[c]) + " ", row_cells[c].style});
    if (detail_w)
      row.push_back({fit(text(get(o, "detail"), ""), detail_w), FAINT});
    out.push_back(runs(std::move(row), w));
  }
  return out;
}

std::vector<Row> side_body(const Json &snap, int w, int h) {
  std::vector<Row> out;
  const Json &c = get(snap, "canary");
  if (truthy(c)) {
    const Json &halted = get(c, "halted");
    bool any = truthy(halted);
    out.push_back(runs({{fit("canary", 9), DIM},
                        {" ", PAPER},
                        {any ? "HALTED" : "ARMED", state_fg(any ? "FAIL" : "WARN")}},
                       w));
    out.push_back(line("orders", ratio(get(c, "orders"), get(c, "max_orders")) + "  fills " +
                                     ratio(get(c, "fills"), get(c, "max_fills")),
                       w));
    out.push_back(line("spend", money(get(c, "spend")) + "/" + money(get(c, "max_spend")) +
                                    "  rounds " + ratio(get(c, "rounds"), get(c, "max_rounds")),
                       w));
    out.push_back(line("risk", money(get(c, "loss")) + "/" + money(get(c, "max_loss")) +
                                   "  awaiting " + count(get(c, "awaiting")),
                       w));
    auto guard = text(get(c, "guard"));
    out.push_back(runs({{fit("latency", 9), DIM},
                        {" ", PAPER},
                        {guard, state_fg(guard == "OK" ? "OK" : "WARN")},
                        {" " + text(get(c, "guard_reason"), ""), FAINT}},
                       w));
    if (halted.is_array()) {
      std::size_t shown = 0;
      for (auto &reason : halted) {
        if (shown++ == 3)
          break;
        // Up to three lines: the reason says how to clear the halt.
        int cw = std::max(1, w - 10);
        auto lines = wrap(text(reason), cw);
        if (lines.size() > 3) {
          std::string rest = lines[2];
          for (std::size_t i = 3; i < lines.size(); ++i)
            rest += " " + lines[i];
          lines.resize(2);
          lines.push_back(fit(rest, cw));
        }
        for (std::size_t i = 0; i < lines.size(); ++i)
          out.push_back(runs({{fit(i == 0 ? "HALT" : "", 9), RED_B},
                              {" ", PAPER},
                              {lines[i], S(Color::red)}},
                             w));
      }
    }
  }
  const Json &net = get(snap, "net");
  if (net.is_object())
    out.push_back(net_row(net, w));
  const Json &health = get(snap, "health");
  if (health.is_array())
    for (auto &row : health) {
      if (!row.is_array() || row.size() < 3)
        continue;
      auto st = text(row[1]);
      out.push_back(runs({{fit(text(row[0]), 9), DIM},
                          {" ", PAPER},
                          {fit(st, 12), state_fg(st)},
                          {" ", PAPER},
                          {text(row[2], ""), FAINT}},
                         w));
    }
  const Json &skips = get(snap, "skips");
  if (skips.is_array() && !skips.empty() && static_cast<int>(out.size()) < h) {
    std::vector<Run> parts{{"skips    ", DIM}};
    std::size_t shown = 0;
    for (auto &row : skips) {
      if (shown++ == 4 || !row.is_array() || row.size() < 2)
        break;
      parts.push_back({text(row[0]) + " " + count(row[1]) + "  ", FAINT});
    }
    out.push_back(runs(std::move(parts), w));
  }
  return out;
}

std::vector<Row> events_body(const Json &events, int w, int h) {
  std::vector<Row> out;
  if (events.is_array())
    for (std::size_t i = 0; i < events.size() && static_cast<int>(out.size()) < h; ++i) {
      const Json &e = events[events.size() - 1 - i]; // newest first
      if (!e.is_array() || e.size() < 4)
        continue;
      auto wall = num(e[0]);
      out.push_back(runs({{(wall ? et_time(*wall) : MISSING) + " ", DIM},
                          {fit(text(e[1]), 9) + " ", S(Color::dim, true)},
                          {text(e[2], ""), level_style(text(e[3], ""))}},
                         w));
    }
  if (out.empty())
    out.push_back(runs({{"  no events", FAINT}}, w));
  return out;
}

std::vector<Row> latency_body(const Json &latency, int w, int max_rows) {
  // TOTALS: only what this mode measures; TRIG>ACK in paper is the model's.
  const std::array<std::pair<const char *, const char *>, 4> totals{{
      {"trigger_to_decision", "TRIG>DEC"},
      {"trigger_to_send", "TRIG>SEND"},
      {"send_to_ack", "NET>ACK"},
      {"trigger_to_ack", "TRIG>ACK"}}};
  std::vector<Row> rows;
  int cw = std::max(6, std::min(9, (w - 11) / 7));
  std::vector<Run> head{{fit("ms", 10), FAINT}};
  for (const char *h : {"LAST", "AVG", "P50", "P95", "P99", "MAX", "n"})
    head.push_back({fit(h, cw, '>'), S(Color::dim, true)});
  rows.push_back(runs(std::move(head), w));
  for (auto &[key, label] : totals) {
    const Json &d = get(latency, key);
    if (!d.is_object())
      continue;
    std::vector<Run> parts{{fit(std::string(label) + (truthy(get(d, "simulated")) ? " sim" : ""), 10), INK_B}};
    for (const char *k : {"last", "avg", "p50", "p95", "p99", "max"})
      parts.push_back({fit(strip_ms(lat(get(d, k))), cw, '>'), INK});
    parts.push_back({fit(count(get(d, "n")), cw, '>'), INK});
    rows.push_back(runs(std::move(parts), w));
  }
  if (rows.size() == 1)
    rows.push_back(runs({{"  no orders measured yet", FAINT}}, w));
  return first(std::move(rows), max_rows);
}

std::vector<Row> quality_body(const Json &q, int w) {
  const Json &last = get(q, "last");
  auto pct = [](const Json &v) {
    auto f = num(v);
    return f ? sf("%.0f%%", *f * 100) : std::string("N/A");
  };
  auto mid = num(get(last, "signal_mid")), slip = num(get(last, "slip_c"));
  auto avg = num(get(q, "avg_slip_c"));
  return {
      runs({{"attempted " + count(get(q, "attempted")) + "  sent " + count(get(q, "sent")) +
                 "  acked " + count(get(q, "acked")) + "  pending " + count(get(q, "pending")),
             INK}},
           w),
      runs({{"filled ", DIM},
            {count(get(q, "filled")), GREEN_B},
            {" partial ", DIM},
            {count(get(q, "partial")), S(Color::green)},
            {" zero ", DIM},
            {count(get(q, "zero_fill")), AMBER},
            {" rej ", DIM},
            {count(get(q, "rejected")), S(Color::red)},
            {" fail ", DIM},
            {count(get(q, "failed")), RED_B}},
           w),
      runs({{"fill rate ", DIM},
            {pct(get(q, "fill_rate")), INK_B},
            {"  reject rate ", DIM},
            {pct(get(q, "reject_rate")), INK}},
           w),
      runs({{"last  signal ", DIM},
            {mid ? commas(*mid, 2) : "N/A", INK},
            {"  ref ask ", DIM},
            {px(get(last, "ask_seen")), INK},
            {"  limit ", DIM},
            {px(get(last, "cap")), INK}},
           w),
      runs({{"      avg fill ", DIM},
            {px(get(last, "price")), INK},
            {"  slip ", DIM},
            {slip ? sf("%+.1fc", *slip) : "N/A",
             pnl_style(slip ? std::optional<double>(-*slip) : std::nullopt)}},
           w),
      runs({{"avg slippage ", DIM},
            {avg ? sf("%+.2fc", *avg) : "N/A", INK},
            {truthy(get(q, "slip_n")) ? "  over " + count(get(q, "slip_n")) + " fills" : "", FAINT}},
           w)};
}

std::vector<Row> latency_band(const Json &snap, int cols, int h) {
  const Json &ex = get(snap, "exec");
  std::string title = std::string("SNIPER EXECUTION LATENCY") +
                      (truthy(get(ex, "paper")) ? " (paper: ACK/fill simulated)" : "");
  int body_h = std::max(0, h - 2);
  if (cols < 100)
    return panel(title, latency_body(get(ex, "latency"), cols - 2, body_h), cols, h);
  auto w = hsplit(cols, {1.6, 1.0}, {66, 34});
  return join({panel(title, latency_body(get(ex, "latency"), w[0] - 2, body_h), w[0], h),
               panel("EXECUTION QUALITY", first(quality_body(get(ex, "quality"), w[1] - 2), body_h), w[1], h)},
              w, h);
}

struct PosColumn {
  const char *name;
  int width;
  char align;
};
std::vector<Row> position_body(const Json &p, int w) {
  std::vector<PosColumn> columns{{"SIDE", 5, '<'},   {"SHARES", 8, '>'}, {"AVG", 6, '>'},
                                 {"COST", 7, '>'},   {"BID", 6, '>'},    {"VALUE", 8, '>'},
                                 {"P/L", 8, '>'}};
  auto total = [&] {
    int n = 0;
    for (auto &c : columns)
      n += c.width;
    return n;
  };
  auto keep = [&](std::initializer_list<const char *> names) {
    columns.erase(std::remove_if(columns.begin(), columns.end(),
                                 [&](const PosColumn &c) {
                                   return !one_of(c.name, names);
                                 }),
                  columns.end());
  };
  if (w < total())
    keep({"SIDE", "SHARES", "COST", "VALUE", "P/L"});
  if (w < total())
    keep({"SIDE", "SHARES", "P/L"});
  auto cells_row = [&](const std::vector<std::pair<std::string, Style>> &values) {
    std::vector<Run> parts;
    for (std::size_t i = 0; i < columns.size(); ++i)
      parts.push_back({(i ? " " : "") + fit(values[i].first, columns[i].width - (i ? 1 : 0), columns[i].align),
                       values[i].second});
    return runs(std::move(parts), w);
  };
  std::vector<std::pair<std::string, Style>> head;
  for (auto &c : columns)
    head.push_back({c.name, S(Color::dim, true)});
  std::vector<Row> out{cells_row(head)};
  Json rows = get(p, "rows");
  if (!rows.is_array() || rows.empty())
    rows = Json::array({Json{{"side", "UP"}}, Json{{"side", "DOWN"}}});
  for (std::size_t i = 0; i < rows.size() && i < 2; ++i) {
    const Json &r = rows[i];
    auto side = text(get(r, "side"));
    auto shares = num(get(r, "shares")), pnl = num(get(r, "pnl"));
    std::vector<std::pair<std::string, Style>> values;
    for (auto &c : columns) {
      std::string name = c.name;
      if (name == "SIDE")
        values.push_back({side, side == "UP" ? GREEN_B : RED_B});
      else if (name == "SHARES")
        values.push_back({shares ? sf("%.3f", *shares) : MISSING, S(Color::ink, shares && *shares)});
      else if (name == "AVG")
        values.push_back({px(get(r, "avg")), INK});
      else if (name == "COST")
        values.push_back({money(get(r, "cost")), INK});
      else if (name == "BID")
        values.push_back({px(get(r, "bid")), INK});
      else if (name == "VALUE")
        values.push_back({money(get(r, "value")), INK});
      else
        values.push_back({money(opt(pnl), true), pnl_style(pnl)});
    }
    out.push_back(cells_row(values));
  }
  const Json &pend = get(p, "pending");
  auto n = num(get(pend, "markets"));
  if (n && *n) {
    auto shares = num(get(pend, "shares"));
    out.push_back(runs({{"awaiting ", DIM},
                        {std::to_string(static_cast<long long>(*n)) + " mkt ", AMBER_B},
                        {sf("%.3fsh ", shares.value_or(0.0)), INK},
                        {"cost " + money(get(pend, "cost")) + " (held at cost until the result)", DIM}},
                       w));
  }
  return out;
}

std::vector<Row> settlement_body(const Json &st, int w, std::string &label) {
  const Json &subject = get(st, "subject"), &last = get(st, "last_settled");
  auto state = text(get(subject, "state"), "DISCOVERING");
  int lw = 13;
  auto row = [&](const char *name, const std::string &value, Style style = INK) {
    return line(name, value, w, style, lw);
  };
  std::vector<Row> rows{row("MARKET STATUS", state, market_state_fg(state))};
  bool winner = truthy(get(subject, "winner"));
  if ((state == PENDING_S || state == "SETTLING" || state == "EXPIRED") && !winner) {
    rows.push_back(row("RESULT",
                       state != "EXPIRED" || !truthy(get(subject, "detail")) ? "WAITING" : "NONE",
                       AMBER_B));
    rows.push_back(row("PENDING AGE", secs(get(subject, "age_s")), INK_B));
    const Json &error = get(subject, "error");
    const Json &why = truthy(error) ? error : get(subject, "detail");
    if (truthy(why))
      rows.push_back(row("WAITING ON", text(why), truthy(error) ? S(Color::red) : FAINT));
    auto pending = num(get(st, "pending_count"));
    if (pending && *pending > 1)
      rows.push_back(row("ALSO PENDING",
                         std::to_string(static_cast<long long>(*pending - 1)) + " more market(s)",
                         AMBER));
  } else if (state == "SETTLING" || state == "SETTLED" || winner) {
    auto who = text(get(subject, "winner"));
    rows.push_back(row("WINNER", who, who == "UP" || who == "DOWN" ? state_fg(who) : INK_B));
    if (state == "SETTLED") {
      auto realized = num(get(subject, "realized"));
      rows.push_back(row("SETTLE PRICE", money(get(subject, "settlement_price"))));
      bool has_position = !get(subject, "has_position").is_boolean() ||
                          get(subject, "has_position").get<bool>();
      rows.push_back(row("REALIZED P/L", has_position ? money(opt(realized), true) : "$0.00 (no position)",
                         pnl_style(realized)));
      rows.push_back(row("SETTLE TIME", secs(get(subject, "settlement_s")), INK_B));
      rows.push_back(row("REDEEM", text(get(subject, "redeem")), FAINT));
    } else
      rows.push_back(row("PENDING AGE", secs(get(subject, "age_s"))));
  } else {
    rows.push_back(row("RESULT", MISSING, FAINT));
    rows.push_back(row("PENDING AGE", MISSING, FAINT));
  }
  if (!truthy(last))
    rows.push_back(row("LAST SETTLED", MISSING, FAINT));
  else if (get(last, "condition_id") == get(subject, "condition_id"))
    rows.push_back(row("LAST SETTLED", "this market", FAINT));
  else {
    auto tail = " " + text(get(last, "winner")) + " " + money(get(last, "realized"), true);
    auto name = text(get(last, "label"));
    if (static_cast<int>(name.size() + tail.size()) > w - lw - 1)
      name = name.substr(0, name.find('-')); // "14:00" when "14:00-14:05" does not fit
    rows.push_back(row("LAST SETTLED", name + tail, pnl_style(num(get(last, "realized")))));
  }
  label = text(get(subject, "label"), "");
  return rows;
}

Row market_line(const Json &v, int w, std::optional<double> now_left = std::nullopt) {
  auto state = text(get(v, "state"));
  auto label_text = text(get(v, "label"));
  bool wide = w >= 33;
  auto label = wide ? fit(label_text, 11) : fit(label_text.substr(0, label_text.find('-')), 5);
  std::string shown;
  std::vector<Run> tail;
  if (state == "ACTIVE") {
    shown = "ACTIVE";
    tail = {{"  ", PAPER}, {now_left ? sf("T-%.0fs", *now_left) : "", S(Color::blue)}};
  } else if (state == PENDING_S || state == "SETTLING" ||
             (state == "EXPIRED" && !truthy(get(v, "detail")))) {
    shown = state != "SETTLING" ? (wide ? "WAITING RESULT" : "WAITING") : "SETTLING";
    tail = {{" ", PAPER}, {secs(get(v, "age_s")), INK_B}};
  } else if (state == "SETTLED") {
    shown = "SETTLED " + text(get(v, "winner")).substr(0, 4);
    auto pnl = num(get(v, "realized"));
    bool has_position = truthy(get(v, "has_position"));
    tail = {{" ", PAPER},
            {has_position ? money(opt(pnl), true) : "no pos", has_position ? pnl_style(pnl) : FAINT}};
  } else
    shown = state + " no result";
  std::vector<Run> parts{{label + " ", INK}, {fit(shown, wide ? 14 : 11), market_state_fg(state)}};
  parts.insert(parts.end(), tail.begin(), tail.end());
  return runs(std::move(parts), w);
}

std::vector<Row> markets_body(const Json &st, const Json &m, int w) {
  std::vector<Row> out;
  const Json &active = get(st, "active");
  out.push_back(runs({{"ACTIVE MARKET", S(Color::dim, true)}}, w));
  out.push_back(active.is_object() ? market_line(active, w, num(get(m, "seconds_left")))
                                   : runs({{"  waiting for discovery", FAINT}}, w));
  const Json &pending = get(st, "pending"), &previous = get(st, "previous");
  if (pending.is_array() && !pending.empty()) {
    out.push_back(runs({{PENDING_S, AMBER_B}}, w));
    for (auto &v : pending)
      out.push_back(market_line(v, w));
  }
  out.push_back(runs({{"PREVIOUS MARKETS", S(Color::dim, true)}}, w));
  if (previous.is_array() && !previous.empty())
    for (auto &v : previous)
      out.push_back(market_line(v, w));
  else
    out.push_back(runs({{"  none yet this session", FAINT}}, w));
  return out;
}

// POSITION | SETTLEMENT | MARKETS. Narrower terminals drop MARKETS, then
// POSITION; SETTLEMENT always stays.
std::vector<Row> settlement_band(const Json &snap, int cols, int h) {
  const Json &st = get(snap, "settlement");
  int body_h = std::max(0, h - 2);
  std::vector<int> w;
  if (cols >= 120)
    w = hsplit(cols, {1.4, 1.0, 1.0}, {50, 34, 36}); // the full POSITION table fits
  else if (cols >= 100)
    w = hsplit(cols, {1.2, 1.0, 1.0}, {38, 34, 28});
  else if (cols >= 70)
    w = hsplit(cols, {1.0, 1.0}, {36, 34});
  else
    w = {cols};
  int settle_w = w.size() > 1 ? w[1] : w[0];
  std::string label;
  auto s_rows = settlement_body(st, settle_w - 2, label);
  auto settle = panel("SETTLEMENT", first(std::move(s_rows), body_h), settle_w, h, label);
  if (w.size() == 1)
    return settle;
  auto pos = panel("POSITION", first(position_body(get(snap, "positions"), w[0] - 2), body_h), w[0], h);
  std::vector<std::vector<Row>> parts{std::move(pos), std::move(settle)};
  if (w.size() == 3)
    parts.push_back(panel("MARKETS", first(markets_body(st, get(snap, "market"), w[2] - 2), body_h), w[2], h));
  return join(parts, w, h);
}

std::vector<Row> build_frame(const Json &snap, int cols, int rows) {
  std::vector<Row> out{header(snap, cols)};
  int remaining = rows - 1;
  if (remaining <= 0)
    return out;
  // top band: TRIGGER | MARKET | ACCOUNT, 8 body rows when there is room
  int top_h = std::min(10, remaining);
  auto w = hsplit(cols, {1.0, 1.1, 0.9}, {26, 30, 24});
  int body_h = std::max(0, top_h - 2);
  const Json &acct = get(snap, "account");
  auto kind = text(get(acct, "kind"), text(get(snap, "mode"), ""));
  add(out, join({panel("TRIGGER", first(trigger_body(get(snap, "trigger"), w[0] - 2), body_h), w[0], top_h),
                 panel("MARKET", first(market_body(get(snap, "market"), w[1] - 2), body_h), w[1], top_h),
                 panel("ACCOUNT " + upper(kind), first(account_body(acct, w[2] - 2), body_h), w[2], top_h)},
                w, top_h));
  remaining -= top_h;
  if (remaining <= 2)
    return out;
  // middle band: ORDERS | CANARY + HEALTH, then POSITION | SETTLEMENT | MARKETS
  int settle_h = remaining >= 15 ? 9 : remaining >= 11 ? 5 : 0;
  int wanted = static_cast<int>(side_body(snap, 1, 99).size()); // what the side panel wants
  int room = remaining - settle_h;
  // Latency only when it leaves the middle band its 8 rows and EVENTS 5.
  int lat_h = room >= 8 + 8 + 5 ? 8 : 0;
  room -= lat_h;
  int mid_h = std::min(room, std::max(8, wanted + 2));
  if (room - mid_h < 5) // not enough for events below: take it all
    mid_h = room;
  auto w2 = hsplit(cols, {1.6, 1.0}, {40, 30});
  int mbody = std::max(0, mid_h - 2);
  add(out, join({panel("ORDERS", orders_body(get(snap, "orders"), w2[0] - 2, mbody), w2[0], mid_h),
                 panel(truthy(get(snap, "canary")) ? "CANARY + HEALTH" : "HEALTH",
                       first(side_body(snap, w2[1] - 2, mbody), mbody), w2[1], mid_h)},
                w2, mid_h));
  remaining -= mid_h;
  if (settle_h) {
    add(out, settlement_band(snap, cols, settle_h));
    remaining -= settle_h;
  }
  if (lat_h) {
    add(out, latency_band(snap, cols, lat_h));
    remaining -= lat_h;
  }
  if (remaining >= 3)
    add(out, panel("EVENTS", events_body(get(snap, "events"), cols - 2, remaining - 2), cols, remaining));
  return out;
}

long long days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long long>(doe) - 719468;
}
int year_of_days(long long z) {
  z += 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  return static_cast<int>(yoe) + static_cast<int>(era * 400) + (mp >= 10 ? 1 : 0);
}
long long floor_div(long long a, long long b) {
  long long q = a / b;
  return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}
// Day number of the n-th Sunday of a month.
long long nth_sunday(int year, unsigned month, int n) {
  long long first_day = days_from_civil(year, month, 1);
  long long weekday = ((first_day + 4) % 7 + 7) % 7; // 0 is Sunday
  return first_day + (7 - weekday) % 7 + (n - 1) * 7;
}
std::string hms(double unix_wall, bool seconds) {
  long long t = static_cast<long long>(std::floor(unix_wall)) + et_offset_s(unix_wall);
  long long day_s = t - floor_div(t, 86400) * 86400;
  int h = static_cast<int>(day_s / 3600), m = static_cast<int>(day_s % 3600 / 60),
      s = static_cast<int>(day_s % 60);
  return seconds ? sf("%02d:%02d:%02d", h, m, s) : sf("%02d:%02d", h, m);
}

// ---------------------------------------------------------- redaction
std::string hide_hex_32(const std::string &in) {
  auto is_hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
  std::string out;
  std::size_t i = 0;
  while (i < in.size()) {
    if (!is_hex(in[i])) {
      out += in[i++];
      continue;
    }
    std::size_t j = i;
    while (j < in.size() && is_hex(in[j]))
      ++j;
    if (j - i == 64) {
      if (out.size() >= 2 && out[out.size() - 2] == '0' &&
          (out.back() == 'x' || out.back() == 'X'))
        out.resize(out.size() - 2);
      out += "<redacted-32-byte-value>";
    } else
      out.append(in, i, j - i);
    i = j;
  }
  return out;
}
std::string compute_terminal_text(std::string_view value, std::size_t max_chars) {
  std::string t = redact(value, max_chars);
  if (max_chars > 0 && t.size() > max_chars)
    t = t.substr(0, max_chars) + "...<truncated>";
  std::string out;
  out.reserve(t.size());
  for (std::size_t i = 0; i < t.size();) {
    auto c = static_cast<unsigned char>(t[i]);
    if (c == '\r' || c == '\n' || c == '\t') {
      out += ' ';
      ++i;
    } else if (c < 0x20 || c == 0x7F) {
      out += '?';
      ++i;
    } else if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
    } else { // one '?' per character outside printable ASCII
      out += '?';
      ++i;
      while (i < t.size() && (static_cast<unsigned char>(t[i]) & 0xC0) == 0x80)
        ++i;
    }
  }
  return out;
}
} // namespace

// ---------------------------------------------------------------- safety
std::string redact(std::string_view value, std::size_t max_chars) {
  std::string t(value);
  if (max_chars > 0 && t.size() > max_chars)
    t = t.substr(0, max_chars) + "...<truncated>";
  // The patterns below only match text holding one of these words.
  auto low = lower(t);
  bool suspect = false;
  for (const char *word : {"://", "auth", "bearer", "key", "secret", "pass", "signature", "eyj"})
    if (low.find(word) != std::string::npos)
      suspect = true;
  if (suspect) {
    static const std::regex url(R"re((https?://)([^/@\s:]+):([^/@\s]+)@)re", std::regex::ECMAScript | std::regex::icase);
    static const std::regex query(
        R"re(([?&](?:access[_-]?token|api[_-]?key|key|secret|signature)=)[^&#\s]+)re",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex named(
        R"re(\b(authorization|bearer|api[_-]?key|secret|passphrase|password|private[_-]?key|poly[_-]?private[_-]?key|signature)\b(\s*[:=]\s*|\s+)(['"]?)([^\s,;'"}\]]+)(\3))re",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex jwt(R"re(eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,})re");
    t = std::regex_replace(t, url, "$1<redacted>:<redacted>@");
    t = std::regex_replace(t, query, "$1<redacted>");
    t = std::regex_replace(t, named, "$1$2$3<redacted>$5");
    t = std::regex_replace(t, jwt, "<redacted-jwt>");
  }
  return hide_hex_32(t);
}

std::string terminal_text(std::string_view value, std::size_t max_chars) {
  // Frames repeat their text, so a short string's answer is cached.
  if (value.size() > 512)
    return compute_terminal_text(value, max_chars);
  thread_local std::unordered_map<std::string, std::string> cache;
  std::string key(value);
  key.push_back('\0');
  key += std::to_string(max_chars);
  auto hit = cache.find(key);
  if (hit != cache.end())
    return hit->second;
  auto out = compute_terminal_text(value, max_chars);
  if (cache.size() >= 8192)
    cache.clear();
  cache.emplace(std::move(key), out);
  return out;
}

// ---------------------------------------------------------------- widgets
std::size_t cells(std::string_view s) {
  std::size_t n = 0;
  for (char c : s)
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80)
      ++n;
  return n;
}

std::size_t width(const Row &row) {
  std::size_t n = 0;
  for (auto &r : row)
    n += cells(r.text);
  return n;
}

std::string fit(std::string_view text_in, int n, char align) {
  if (n <= 0)
    return {};
  auto limit = static_cast<std::size_t>(n);
  auto have = cells(text_in);
  std::string t = have <= limit ? std::string(text_in)
                  : n > 1       ? take(text_in, limit - 1) + G_ELLIPSIS
                                : take(text_in, limit);
  auto shown = std::min(have, limit);
  std::string space(limit - shown, ' ');
  return align == '>' ? space + t : t + space;
}

Row pad(Row row, int cols, Style style) {
  cols = std::max(0, cols);
  auto w = static_cast<int>(width(row));
  if (w == cols)
    return row;
  if (w < cols) {
    row.push_back({std::string(static_cast<std::size_t>(cols - w), ' '), style});
    return row;
  }
  Row out;
  int left = cols;
  for (auto &r : row) {
    if (left <= 0)
      break;
    auto n = static_cast<int>(cells(r.text));
    if (n <= left) {
      out.push_back(r);
      left -= n;
    } else {
      out.push_back({take(r.text, static_cast<std::size_t>(left)), r.style});
      left = 0;
    }
  }
  return out;
}

std::vector<Row> panel(const std::string &title, const std::vector<Row> &body,
                       int cols, int rows, const std::string &right_note) {
  if (cols < 4 || rows < 2) {
    std::vector<Row> blank;
    for (int i = 0; i < std::max(0, rows); ++i)
      blank.push_back(pad({}, cols));
    return blank;
  }
  int inner = cols - 2;
  std::string cap = " " + take(title, static_cast<std::size_t>(std::max(0, inner - 4))) + " ";
  std::string note = right_note.empty() ? "" : " " + right_note + " ";
  int fill = inner - 1 - static_cast<int>(cells(cap)) - static_cast<int>(cells(note));
  Row top{{G_TL, RULE}, {G_H, RULE}, {cap, INK_B}};
  if (fill > 0)
    top.push_back({repeat(G_H, fill), RULE});
  if (!note.empty())
    top.push_back({note, FAINT});
  top.push_back({G_TR, RULE});
  std::vector<Row> out{pad(std::move(top), cols)};
  for (int i = 0; i < rows - 2; ++i) {
    Row content = i < static_cast<int>(body.size()) ? body[static_cast<std::size_t>(i)] : Row{};
    Row row{{G_V, RULE}};
    for (auto &r : pad(std::move(content), inner))
      row.push_back(std::move(r));
    row.push_back({G_V, RULE});
    out.push_back(pad(std::move(row), cols));
  }
  out.push_back(pad({{G_BL, RULE}, {repeat(G_H, inner), RULE}, {G_BR, RULE}}, cols));
  return out;
}

std::vector<int> hsplit(int total, const std::vector<double> &weights,
                        const std::vector<int> &mins) {
  std::size_t n = weights.size();
  if (n == 0)
    return {};
  std::vector<int> out = mins;
  int sum = 0;
  for (int m : mins)
    sum += m;
  int spare = total - sum;
  if (spare <= 0) {
    // Not enough room: shrink from the right but never below 3.
    for (int i = static_cast<int>(n) - 1; spare < 0 && i >= 0; --i) {
      auto &slot = out[static_cast<std::size_t>(i)];
      int cut = std::min(slot - 3, -spare);
      if (cut > 0) {
        slot -= cut;
        spare += cut;
      }
    }
    return out;
  }
  double wsum = 0;
  for (double x : weights)
    wsum += x;
  if (wsum == 0)
    wsum = 1;
  int used = 0;
  for (std::size_t i = 0; i < n; ++i) {
    out[i] += static_cast<int>(spare * weights[i] / wsum);
    used += out[i];
  }
  out.back() += total - used;
  return out;
}

std::vector<Row> join(const std::vector<std::vector<Row>> &parts,
                      const std::vector<int> &widths, int rows) {
  std::vector<Row> out;
  for (int r = 0; r < rows; ++r) {
    Row row;
    for (std::size_t p = 0; p < parts.size() && p < widths.size(); ++p) {
      Row cell = r < static_cast<int>(parts[p].size()) ? parts[p][static_cast<std::size_t>(r)] : Row{};
      for (auto &run : pad(std::move(cell), widths[p]))
        row.push_back(std::move(run));
    }
    out.push_back(std::move(row));
  }
  return out;
}

Row bar(double frac, int w, Style fill) {
  if (w <= 0)
    return {};
  if (!std::isfinite(frac))
    frac = 0;
  frac = std::clamp(frac, 0.0, 1.0);
  int units = static_cast<int>(std::lround(frac * w * 8));
  int full = std::min(units / 8, w), rem = units % 8;
  Row out;
  if (full)
    out.push_back({repeat(G_BLOCK, full), fill});
  if (rem && full < w) {
    out.push_back({EIGHTHS[static_cast<std::size_t>(rem - 1)], fill});
    ++full;
  }
  if (full < w)
    out.push_back({repeat(G_LIGHT, w - full), FAINT});
  return out;
}

// ---------------------------------------------------------------- clock
int et_offset_s(double unix_wall) {
  auto t = static_cast<long long>(std::floor(unix_wall));
  int year = year_of_days(floor_div(t - 5 * 3600, 86400));
  long long start = nth_sunday(year, 3, 2) * 86400 + 7 * 3600; // 2am EST
  long long end = nth_sunday(year, 11, 1) * 86400 + 6 * 3600;  // 2am EDT
  return t >= start && t < end ? -4 * 3600 : -5 * 3600;
}
std::string et_time(double unix_wall) { return hms(unix_wall, true); }
std::string et_clock(double unix_wall) { return hms(unix_wall, true) + " ET"; }
std::string et_label(double start, double end) {
  return hms(start, false) + "-" + hms(end, false);
}

// ---------------------------------------------------------------- layout
std::vector<Row> build(const Json &snap, int cols, int rows) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  std::vector<Row> frame;
  try {
    frame = build_frame(snap, cols, rows);
  } catch (const std::exception &e) {
    // The dashboard must never take the bot down.
    frame = {pad({{fit(" dashboard layout error: " + terminal_text(e.what(), 200), cols), RED_B}}, cols)};
  }
  std::vector<Row> out;
  for (auto &row : frame) {
    if (static_cast<int>(out.size()) == rows)
      break;
    out.push_back(pad(std::move(row), cols));
  }
  while (static_cast<int>(out.size()) < rows)
    out.push_back(pad({}, cols));
  return out;
}

std::string plain_line(const Json &snap) {
  const Json &t = get(snap, "trigger"), &m = get(snap, "market"), &a = get(snap, "account");
  auto left = num(get(m, "seconds_left"));
  bool live = text(get(a, "kind"), "") == "LIVE";
  auto mid = num(get(t, "mid"));
  return "[DASH] " + text(get(snap, "mode")) + " " + text(get(m, "slug"), "no-round") + " " +
         (left ? sf("T-%.0fs", *left) : std::string("T-?")) + " btc=" + text(get(t, "status")) +
         " mid=" + (mid ? sf("%.2f", *mid) : MISSING) + " signals=" + count(get(t, "signals")) +
         " orders=" + count(get(t, "orders")) + " filled=" + count(get(t, "filled")) +
         " pnl=" + money(get(a, live ? "realized" : "total"), true);
}

// ---------------------------------------------------------------- output
std::string sgr(Style style, ColorMode mode) {
  std::string out = "\x1b[0";
  if (style.bold)
    out += ";1";
  if (mode == ColorMode::mono)
    return out + "m";
  static const std::array<std::array<int, 3>, 15> rgb{{
      {244, 239, 228}, {236, 230, 216}, {26, 26, 28},   {128, 122, 110}, {176, 169, 155},
      {74, 70, 62},    {0, 118, 51},    {0, 156, 68},   {186, 28, 42},   {222, 60, 60},
      {176, 122, 0},   {28, 78, 168},   {108, 58, 158}, {0, 116, 128},   {255, 253, 248}}};
  static const std::array<int, 15> xterm{230, 224, 234, 244, 250, 238, 28, 34,
                                         124, 160, 136, 25,  91,  30,  231};
  auto fg = static_cast<std::size_t>(style.fg), bg = static_cast<std::size_t>(style.bg);
  if (mode == ColorMode::truecolor)
    out += sf(";38;2;%d;%d;%d;48;2;%d;%d;%d", rgb[fg][0], rgb[fg][1], rgb[fg][2],
              rgb[bg][0], rgb[bg][1], rgb[bg][2]);
  else
    out += sf(";38;5;%d;48;5;%d", xterm[fg], xterm[bg]);
  return out + "m";
}

std::string render_row(const Row &row, ColorMode mode) {
  std::string out;
  std::optional<Style> last;
  for (auto &r : row) {
    if (r.text.empty())
      continue;
    if (!last || !(*last == r.style)) {
      out += sgr(r.style, mode);
      last = r.style;
    }
    out += r.text;
  }
  return out + "\x1b[0m";
}
} // namespace sniper::dash
