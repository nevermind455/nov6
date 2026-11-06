#include "sniper/paper.hpp"
#include <algorithm>
#include <cmath>

namespace sniper {
using py::D;
using py::Dec;
using py::Rounding;
namespace {
const Dec ZERO = D(0), ONE = D(1);
const Dec MONEY = D("0.000001"), FEE_PRECISION = D("0.00001");
const Dec CENT = D("0.01");
const Dec MIN_MARKETABLE_BUY_USD = D(1);

struct DecLess {
  bool operator()(const Dec &a, const Dec &b) const { return py::cmp(a, b) < 0; }
};
Dec dec_of(double v, const std::string &name) {
  if (!std::isfinite(v))
    throw PaperRejected("invalid " + name + ": " + py::repr(v));
  return D(v);
}
std::optional<Dec> max_of(const std::vector<Dec> &v) {
  if (v.empty())
    return std::nullopt;
  Dec m = v[0];
  for (std::size_t i = 1; i < v.size(); ++i)
    if (v[i] > m)
      m = v[i];
  return m;
}
Dec tick_cap(const Dec &configured, const Dec &tick) {
  Dec cap = py::min(configured, ONE - tick);
  return py::to_integral(cap / tick, Rounding::Floor) * tick;
}
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}
Dec venue_minimum_stake(const Dec &wanted, const DecLevels &asks,
                        const Dec &minimum, const Dec &cap) {
  if (minimum <= ZERO || cap <= ZERO || asks.empty())
    return wanted;
  if (asks[0].first <= ZERO || asks[0].first > cap)
    return wanted;
  Dec remaining = minimum, notional = ZERO;
  for (auto &[price, size] : asks) {
    if (price > cap)
      break;
    if (price <= ZERO || size <= ZERO)
      continue;
    Dec take = remaining <= size ? remaining : size;
    notional = notional + take * price;
    remaining = remaining - take;
    if (remaining <= ZERO)
      break;
  }
  if (remaining > ZERO)
    return wanted;
  Dec headroom = D(0.04);
  Dec average = notional / minimum;
  Dec room = py::min(headroom, py::max(ZERO, cap - average));
  Dec required = py::quantize(notional + minimum * room, CENT, Rounding::Up);
  return wanted >= required ? wanted : required;
}
} // namespace

BookSnapshot snapshot_from_book_view(const BookView &view,
                                     const std::optional<std::string> &expected,
                                     double now_mono, double venue_wall_now) {
  if (expected && view.token != *expected)
    throw PaperRejected("public order book token does not match this order");
  BookSnapshot b;
  b.token_id = view.token;
  std::map<Dec, Dec, DecLess> agg;
  std::vector<Dec> order;
  for (auto &[p, q] : view.asks) {
    Dec price = dec_of(p, "ask price"), size = dec_of(q, "ask size");
    if (!(ZERO < price && price < ONE) || size <= ZERO)
      throw PaperRejected("public order book has an out-of-range ask level");
    auto it = agg.find(price);
    if (it == agg.end())
      agg.emplace(price, size);
    else
      it->second = it->second + size;
  }
  for (auto &[p, q] : agg)
    b.asks.emplace_back(p, q);
  std::map<Dec, Dec, DecLess> bagg;
  for (auto &[p, q] : view.bids) {
    Dec price = dec_of(p, "bid price"), size = dec_of(q, "bid size");
    if (!(ZERO < price && price < ONE) || size <= ZERO)
      throw PaperRejected("public order book has an out-of-range bid level");
    auto it = bagg.find(price);
    if (it == bagg.end())
      bagg.emplace(price, size);
    else
      it->second = it->second + size;
  }
  for (auto it = bagg.rbegin(); it != bagg.rend(); ++it)
    b.bids.emplace_back(it->first, it->second);
  if (auto bb = view.best_bid()) {
    b.best_bid = dec_of(*bb, "bid");
    if (!(ZERO < *b.best_bid && *b.best_bid < ONE))
      throw PaperRejected("public order book has an out-of-range bid");
  }
  if (!b.asks.empty() && b.best_bid && *b.best_bid >= b.asks[0].first)
    throw PaperRejected("public order book is crossed or locked");
  if (!view.exchange_ts_ms)
    throw PaperRejected("websocket order book omitted exchange timestamp");
  if (*view.exchange_ts_ms <= 0)
    throw PaperRejected("websocket order book has an invalid exchange timestamp");
  if (view.tick_size) {
    b.tick_size = dec_of(*view.tick_size, "tick");
    if (!(ZERO < *b.tick_size && *b.tick_size < ONE))
      throw PaperRejected("websocket order book has an invalid tick size");
  }
  b.timestamp = std::to_string(*view.exchange_ts_ms);
  if (view.hash && !view.hash->empty())
    b.book_hash = view.hash;
  if (view.updated_mono) {
    double age = now_mono - *view.updated_mono;
    if (!std::isfinite(age) || age < 0 || age >= 86400)
      throw PaperRejected("websocket order book has an invalid receipt time");
    b.received_wall = venue_wall_now - age;
  } else
    b.received_wall = venue_wall_now;
  return b;
}

Dec curve_fee(const Dec &shares, const Dec &price, const MarketRules &rules) {
  Dec raw =
      shares * rules.fee_rate * py::pow(price * (ONE - price), rules.fee_exponent);
  return py::quantize(raw, FEE_PRECISION, Rounding::HalfUp);
}

FillQuote estimate_fok(const BookSnapshot &book, const Dec &amount,
                       const Dec &max_price, const MarketRules &rules,
                       const Dec &min_price,
                       const std::optional<Dec> &min_shares) {
  Dec wanted = py::quantize(amount, MONEY, Rounding::HalfUp);
  const Dec &configured_cap = max_price, &configured_floor = min_price;
  if (wanted <= ZERO)
    throw PaperRejected("paper amount must be positive");
  if (!(ZERO < configured_cap && configured_cap < ONE))
    throw PaperRejected("maximum buy price must be between 0 and 1");
  if (configured_floor < ZERO || configured_floor >= configured_cap)
    throw PaperRejected("minimum buy price must be below the maximum buy price");
  if (!rules.tick_size || !(ZERO < *rules.tick_size && *rules.tick_size < ONE))
    throw PaperRejected("paper market has no valid venue tick size");
  const Dec tick = *rules.tick_size;
  if (book.tick_size && *book.tick_size != tick)
    throw PaperRejected("public book and market rules disagree on tick size");
  Dec cap = tick_cap(configured_cap, tick);
  if (cap <= ZERO)
    throw PaperRejected("maximum buy price is below one venue tick");
  Dec floor = configured_floor <= ZERO
                  ? ZERO
                  : py::to_integral(configured_floor / tick, Rounding::Ceiling) *
                        tick;
  if (book.asks.empty())
    throw PaperRejected("cannot FOK buy: no asks on the live book");
  if (book.asks[0].first < floor)
    throw PaperRejected("best ask " + book.asks[0].first.str() +
                        " is below MIN_BUY_PRICE " + floor.str());
  Dec remaining = wanted, total_shares = ZERO, total_fee = ZERO, worst = ZERO;
  FillQuote q;
  for (auto &[price, available] : book.asks) {
    if (price > cap)
      break;
    Dec available_notional = price * available;
    Dec spend = py::min(remaining, available_notional);
    if (spend <= ZERO)
      continue;
    Dec shares = spend / price;
    q.levels.emplace_back(price, shares, spend);
    total_shares = total_shares + shares;
    total_fee = total_fee + curve_fee(shares, price, rules);
    remaining = remaining - spend;
    worst = price;
    if (remaining <= MONEY / D(2)) {
      remaining = ZERO;
      break;
    }
  }
  if (remaining > ZERO) {
    Dec available = wanted - remaining;
    throw PaperRejected("FOK no-fill: only $" + available.fixed(6) +
                        " available at or below " + cap.str());
  }
  std::vector<Dec> mins;
  if (book.min_order_size)
    mins.push_back(*book.min_order_size);
  if (rules.min_order_size)
    mins.push_back(*rules.min_order_size);
  auto minimum = max_of(mins);
  if (min_shares)
    minimum = *min_shares;
  if (minimum && total_shares < *minimum)
    throw PaperRejected("order size " + total_shares.fixed(6) +
                        " shares is below venue minimum " + minimum->str());
  total_fee = py::quantize(total_fee, FEE_PRECISION, Rounding::HalfUp);
  q.notional = wanted;
  q.shares = total_shares;
  q.average_price = wanted / total_shares;
  q.worst_price = worst;
  q.fee = total_fee;
  return q;
}

FillQuote estimate_fak(const BookSnapshot &book, const Dec &amount,
                       const Dec &max_price, const MarketRules &rules,
                       const Dec &min_price,
                       const std::optional<Dec> &min_shares) {
  try {
    return estimate_fok(book, amount, max_price, rules, min_price, min_shares);
  } catch (const PaperRejected &e) {
    if (lower(e.what()).find("fok no-fill") == std::string::npos)
      throw;
  }
  Dec wanted = py::quantize(amount, MONEY, Rounding::HalfUp);
  const Dec tick = *rules.tick_size;
  Dec cap = tick_cap(max_price, tick);
  Dec remaining = wanted, spent = ZERO, shares = ZERO, fee = ZERO, worst = ZERO;
  FillQuote q;
  for (auto &[price, available] : book.asks) {
    if (price > cap)
      break;
    Dec spend = py::min(remaining, price * available);
    if (spend <= ZERO)
      continue;
    Dec got = spend / price;
    q.levels.emplace_back(price, got, spend);
    shares = shares + got;
    fee = fee + curve_fee(got, price, rules);
    spent = spent + spend;
    remaining = remaining - spend;
    worst = price;
  }
  if (shares <= ZERO)
    throw PaperRejected("FAK no-fill: nothing at or below " + cap.str());
  q.notional = spent;
  q.shares = shares;
  q.average_price = spent / shares;
  q.worst_price = worst;
  q.fee = py::quantize(fee, FEE_PRECISION, Rounding::HalfUp);
  return q;
}

FillQuote estimate_fak_shares(const BookSnapshot &book, const Dec &shares,
                              const Dec &max_price, const MarketRules &rules,
                              const Dec &min_price,
                              const std::optional<Dec> &min_shares) {
  const Dec &wanted = shares;
  if (wanted <= ZERO)
    throw PaperRejected("paper share count must be positive");
  if (!(ZERO < max_price && max_price < ONE))
    throw PaperRejected("maximum buy price must be between 0 and 1");
  if (min_price < ZERO || min_price >= max_price)
    throw PaperRejected("minimum buy price must be below the maximum buy price");
  if (!rules.tick_size || !(ZERO < *rules.tick_size && *rules.tick_size < ONE))
    throw PaperRejected("paper market has no valid venue tick size");
  const Dec tick = *rules.tick_size;
  if (book.tick_size && *book.tick_size != tick)
    throw PaperRejected("public book and market rules disagree on tick size");
  Dec cap = tick_cap(max_price, tick);
  if (cap <= ZERO)
    throw PaperRejected("maximum buy price is below one venue tick");
  Dec spend = wanted * cap;
  if (spend != py::quantize(spend, CENT))
    throw PaperRejected(
        "share-sized stake is not a whole-cent amount at the price cap");
  if (book.asks.empty())
    throw PaperRejected("FAK no-fill: no asks on the book at the match");
  return estimate_fak(book, spend, cap, rules, min_price, min_shares);
}

LiveOrderCheck live_presend_check(const Plan &plan, const MarketRules &rules,
                                  const std::optional<Dec> &order_tick,
                                  const std::optional<Dec> &order_minimum,
                                  double max_buy_price, double min_buy_price,
                                  double max_spread, double now_mono,
                                  double venue_wall_now) {
  LiveOrderCheck out;
  // As paper: an order's cap may tighten the account ceiling, never loosen it.
  double cap = std::min(plan.cap, max_buy_price);
  // lean_view: the asks up to the cap (at least the best ask) and the best
  // bid, which is all the checks read
  BookView view = plan.view;
  std::size_t n = 0;
  while (n < view.asks.size() && view.asks[n].first <= cap + 1e-12)
    ++n;
  view.asks.resize(std::min(view.asks.size(), std::max<std::size_t>(n, 1)));
  if (view.bids.size() > 1)
    view.bids.resize(1);
  BookSnapshot book =
      snapshot_from_book_view(view, plan.token, now_mono, venue_wall_now);
  auto differs = [](const std::optional<Dec> &a, const std::optional<Dec> &b) {
    return a.has_value() != b.has_value() || (a && *a != *b);
  };
  if (differs(rules.tick_size, order_tick) ||
      differs(rules.min_order_size, order_minimum) ||
      (book.tick_size && differs(book.tick_size, rules.tick_size)))
    throw PaperRejected("market rules changed; background refresh requested");
  if (book.asks.empty() || !book.best_bid)
    throw PaperRejected("one-sided book");
  if (book.asks[0].first - *book.best_bid > D(max_spread))
    throw PaperRejected("spread exceeds the limit");
  if (!rules.tick_size || !(ZERO < *rules.tick_size && *rules.tick_size < ONE))
    throw PaperRejected("market has no valid venue tick size");
  const Dec tick = *rules.tick_size;
  Dec price = py::to_integral(D(cap) / tick, Rounding::Floor) * tick;
  if (!plan.shares)
    throw PaperRejected("a live order is share-sized");
  // Whole shares at the limit determine the fixed maker stake.
  Dec size = D(static_cast<long long>(*plan.shares));
  std::vector<Dec> mins;
  if (book.min_order_size)
    mins.push_back(*book.min_order_size);
  if (rules.min_order_size)
    mins.push_back(*rules.min_order_size);
  if (plan.venue_min_shares) {
    out.min_override = D(static_cast<long long>(*plan.venue_min_shares));
    mins = {*out.min_override};
  }
  if (auto m = max_of(mins); m && size < *m)
    throw PaperRejected("order size " + size.str() +
                        " shares is below venue minimum " + m->str());
  Dec spend = size * price;
  if (spend != py::quantize(spend, CENT))
    throw PaperRejected(size.str() + " shares at " + price.str() +
                        " is not a whole-cent amount");
  if (spend < MIN_MARKETABLE_BUY_USD)
    throw PaperRejected("invalid amount for a marketable BUY order ($" +
                        spend.fixed(2) + "), min size: $1");
  out.quote = estimate_fak_shares(book, size, D(price.to_double()), rules,
                                  D(min_buy_price), out.min_override);
  out.cap = D(cap);
  out.price = price;
  out.size = size;
  out.spend = spend;
  return out;
}

PaperBroker::PaperBroker(PaperConfig c, PaperHooks h)
    : cfg(c), h_(std::move(h)) {}

bool PaperBroker::has_recent_own_fill(const std::string &token) {
  if (!(cfg.own_fill_ttl_s > 0))
    return false;
  auto it = own_.find(token);
  return it != own_.end() && !it->second.empty() &&
         h_.mono() - std::get<0>(it->second.back()) < cfg.own_fill_ttl_s;
}

BookSnapshot PaperBroker::net_of_own_fills(const BookSnapshot &book) {
  if (!(cfg.own_fill_ttl_s > 0))
    return book;
  double now = h_.mono();
  auto it = own_.find(book.token_id);
  std::vector<std::tuple<double, Dec, Dec>> live;
  if (it != own_.end()) {
    for (auto &f : it->second)
      if (now - std::get<0>(f) < cfg.own_fill_ttl_s)
        live.push_back(f);
    if (live.empty())
      own_.erase(it);
    else
      it->second = live;
  }
  if (live.empty())
    return book;
  std::map<Dec, Dec, DecLess> used;
  for (auto &[t, price, shares] : live) {
    auto u = used.find(price);
    if (u == used.end())
      used.emplace(price, shares);
    else
      u->second = u->second + shares;
  }
  BookSnapshot out = book;
  out.asks.clear();
  for (auto &[p, sz] : book.asks) {
    auto u = used.find(p);
    Dec left = u == used.end() ? sz - ZERO : sz - u->second;
    if (left > ZERO)
      out.asks.emplace_back(p, left);
  }
  return out;
}

std::optional<PxLevels> PaperBroker::net_asks(const BookView &view) {
  try {
    auto snap = snapshot_from_book_view(view, view.token, h_.mono(),
                                        h_.venue_wall());
    auto net = net_of_own_fills(snap);
    PxLevels out;
    for (auto &[p, q] : net.asks)
      out.emplace_back(p.to_double(), q.to_double());
    return out;
  } catch (const PaperRejected &) {
    return std::nullopt;
  }
}

std::pair<double, double>
PaperBroker::check_book(const BookSnapshot &book, const std::string &side,
                        const std::optional<std::string> &token,
                        bool require_asks) {
  if (token && book.token_id != *token)
    throw PaperRejected("public order book token does not match the order");
  if (!book.timestamp)
    throw PaperRejected("public order book omitted exchange timestamp");
  if (!std::isfinite(book.received_wall) || book.received_wall <= 0)
    throw PaperRejected("public order book omitted receipt time");
  std::optional<double> ts;
  try {
    ts = parse_exchange_ts(py::parse_float(*book.timestamp));
  } catch (...) {
    ts.reset();
  }
  if (!ts)
    throw PaperRejected("public order book has an invalid timestamp");
  double now_wall = h_.venue_wall();
  double quiet = now_wall - *ts, held = now_wall - book.received_wall;
  if (quiet < -cfg.future_tol_s)
    throw PaperRejected("public order book is future-dated (age=" +
                        py::fixed(quiet, 3) + "s)");
  if (held < -cfg.future_tol_s)
    throw PaperRejected("public order book receipt is future-dated");
  if (held > cfg.max_book_age_s)
    throw PaperRejected("public order book is stale in hand (held=" +
                        py::fixed(held, 3) + "s)");
  if (quiet > cfg.max_quiet_s)
    throw PaperRejected("public order book has not changed for " +
                        py::fixed(quiet, 3) + "s");
  if (require_asks && book.asks.empty())
    throw PaperRejected("cannot FOK buy " + side +
                        ": no asks on the live book (liquidity pulled or "
                        "one-sided)");
  return {quiet, held};
}

FillQuote PaperBroker::quote(const BookSnapshot &book, const Order &o) {
  if (o.fak)
    return estimate_fak(book, o.spend, o.cap, o.rules, o.floor, o.min_override);
  return estimate_fok(book, o.spend, o.cap, o.rules, o.floor, o.min_override);
}

void PaperBroker::reject(Order &o, const std::string &why) {
  ++rejected_orders;
  o.result.filled = false;
  o.result.error = why.empty() ? "paper order rejected" : why;
  o.finished = true;
  if (h_.audit)
    h_.audit(Json{{"status", "REJECTED"},
                  {"side", o.side},
                  {"requested_amount", o.plan ? o.plan->amount : 0.0},
                  {"reason", o.result.error},
                  {"wall", h_.unix_wall()},
                  {"cash_after", h_.cash_balance()}});
}

void PaperBroker::begin(const std::shared_ptr<Plan> &plan,
                        const SniperSettings &s, BookState &books) {
  Order o;
  o.plan = plan;
  o.side = plan->side;
  o.fak = s.order_type == "FAK";
  o.latency_ms = plan->lat;
  o.end = plan->end;
  std::string token = plan->side == "UP" ? plan->up : plan->down;
  o.token = token;
  if (!plan->sign_error.empty()) {
    // _fire_now refused before the broker: an ordinary refusal
    o.result.error = plan->sign_error;
    o.finished = true;
    orders_.push_back(std::move(o));
    return;
  }
  bool slot = busy_.insert(token).second;
  o.owns_slot = slot;
  try {
    if (!slot)
      throw PaperRejected(
          "another paper order is already in flight on this token");
    if (plan->shares && !o.fak)
      throw PaperRejected("a share-sized order is a fill-and-kill");
    if (token.empty())
      throw PaperRejected("missing token id");
    if (!std::isfinite(o.end) || !(o.latency_ms >= 0 && o.latency_ms < 60000))
      throw PaperRejected(!std::isfinite(o.end)
                              ? "missing current round end timestamp"
                              : "invalid per-order latency");
    double now = h_.unix_wall();
    if (!(o.end - cfg.trade_window_seconds <= now &&
          now < o.end - cfg.min_seconds_to_expiry))
      throw PaperRejected(
          "paper order is outside the current round execution interval");
    auto [cond, up, down] = h_.context();
    if (cond.empty())
      throw PaperRejected("missing current condition id; cannot settle paper PnL");
    if (!plan->cond.empty() && plan->cond != cond)
      throw PaperRejected("condition does not belong to the current paper round");
    std::string expected = plan->side == "UP" ? up : down;
    if (!expected.empty() && token != expected)
      throw PaperRejected("token does not belong to the current paper round");
    o.cond = cond;
    o.up = up;
    o.down = down;
    const MarketRules *rules = h_.rules(cond);
    if (!rules)
      throw PaperRejected("cannot read live market rules: unavailable");
    o.rules = *rules;
    if (o.rules.up_token_id.value_or("") != up ||
        o.rules.down_token_id.value_or("") != down)
      throw PaperRejected("Gamma and CLOB disagree on UP/DOWN token mapping");
    if (!o.rules.taker_delay_ms)
      throw PaperRejected(
          "venue matching delay is unknown; realistic paper fill is impossible");
    o.cutoff = o.end - cfg.min_seconds_to_expiry;
    if (h_.unix_wall() + o.latency_ms / 1000.0 >= o.cutoff)
      throw PaperRejected(
          "not enough time remaining for paper latency before cutoff");
    double cap = cfg.max_buy_price;
    cap = std::min(cap, plan->cap);
    if (cap <= cfg.min_buy_price)
      throw PaperRejected("order price cap " + py::repr(cap) +
                          " is at or below the floor " +
                          py::repr(cfg.min_buy_price));
    o.cap = D(cap);
    o.floor = D(cfg.min_buy_price);
    // the decision book: the live websocket view
    BookView view = books.view(token);
    if (view.asks.empty() || view.status != LIVE)
      throw PaperRejected("public order book unavailable: no live websocket "
                          "book with asks (REST fallback not supported)");
    BookSnapshot decision;
    try {
      decision = snapshot_from_book_view(view, token, h_.mono(), h_.venue_wall());
    } catch (const PaperRejected &) {
      throw PaperRejected("public order book unavailable: websocket book "
                          "refused (REST fallback not supported)");
    }
    if (decision.asks.empty())
      throw PaperRejected("public order book unavailable: no asks");
    decision = net_of_own_fills(decision);
    if (decision.tick_size && o.rules.tick_size &&
        *decision.tick_size != *o.rules.tick_size)
      o.rules.tick_size = decision.tick_size; // refreshed rules (_rules_matching)
    check_book(decision, o.side, token);
    if (!decision.best_bid)
      throw PaperRejected("public order book has no bid; spread is unknown");
    Dec spread = decision.asks[0].first - *decision.best_bid;
    if (spread > D(cfg.max_spread))
      throw PaperRejected("spread " + spread.fixed(6) +
                          " exceeds paper limit " + py::fixed(cfg.max_spread, 6));
    if (plan->shares) {
      Dec size = D(static_cast<long long>(*plan->shares));
      const Dec tick = *o.rules.tick_size;
      Dec limit = tick_cap(D(cap), tick);
      std::vector<Dec> mins;
      if (decision.min_order_size)
        mins.push_back(*decision.min_order_size);
      if (o.rules.min_order_size)
        mins.push_back(*o.rules.min_order_size);
      if (plan->venue_min_shares) {
        o.min_override = D(static_cast<long long>(*plan->venue_min_shares));
        mins = {*o.min_override};
      }
      if (size != py::to_integral(size) || size <= ZERO)
        throw PaperRejected("a share-sized order is whole shares");
      if (auto m = max_of(mins); m && size < *m)
        throw PaperRejected("order size " + size.str() +
                            " shares is below venue minimum " + m->str());
      o.spend = size * limit;
      if (o.spend != py::quantize(o.spend, CENT))
        throw PaperRejected(
            "share-sized stake is not a whole-cent amount at the price cap");
      o.size = size;
    } else {
      Dec wanted = py::quantize(D(plan->amount), CENT, Rounding::HalfUp);
      std::vector<Dec> mins;
      if (decision.min_order_size)
        mins.push_back(*decision.min_order_size);
      if (o.rules.min_order_size)
        mins.push_back(*o.rules.min_order_size);
      o.spend = wanted;
      auto tick = o.rules.tick_size;
      if (auto m = max_of(mins);
          m && !decision.asks.empty() && tick && ZERO < *tick && *tick < ONE) {
        Dec vcap = tick_cap(D(cap), *tick);
        o.spend = venue_minimum_stake(wanted, decision.asks, *m, vcap);
        // size_at_limit: minimum_spend_at_limit
        Dec at_limit = py::quantize(*m * vcap, CENT, Rounding::Ceiling);
        o.spend = py::max(o.spend, at_limit);
      }
    }
    if (o.spend < MIN_MARKETABLE_BUY_USD)
      throw PaperRejected("invalid amount for a marketable BUY order ($" +
                          o.spend.fixed(2) + "), min size: $" +
                          MIN_MARKETABLE_BUY_USD.str());
    if (plan->hybrid && plan->risk_budget_usd) {
      try {
        hybrid::enforce_order_budget(o.spend, o.rules.fee_rate,
                                     D(*plan->risk_budget_usd));
      } catch (const py::ValueError &e) {
        throw PaperRejected(std::string("hybrid risk check: ") + e.what());
      }
    }
    if (o.size)
      estimate_fak_shares(decision, *o.size, o.cap, o.rules, o.floor,
                          o.min_override);
    else if (o.fak)
      estimate_fak(decision, o.spend, o.cap, o.rules, o.floor);
    else
      estimate_fok(decision, o.spend, o.cap, o.rules, o.floor);
    o.wake_mono = plan->sent_mono + o.latency_ms / 1000.0;
    o.arrival_ms = plan->sent_wall * 1000.0 + o.latency_ms + s.venue_offset_ms;
    o.arrival_mono = plan->sent_mono + o.latency_ms / 1000.0;
  } catch (const PaperRejected &e) {
    reject(o, e.what());
  }
  if (o.finished && slot) {
    busy_.erase(token);
    o.owns_slot = false;
  }
  orders_.push_back(std::move(o));
}

std::optional<double> PaperBroker::next_wake() const {
  std::optional<double> r;
  for (auto &o : orders_) {
    double t = o.finished ? 0
                          : (o.wait_started < 0 ? o.wake_mono
                                                : o.wait_started + 0.005);
    if (!r || t < *r)
      r = t;
  }
  return r;
}

void PaperBroker::arrive(Order &o, BookState &books, const SniperSettings &s) {
  double now = h_.mono();
  if (o.wait_started < 0) {
    if (h_.unix_wall() >= o.cutoff) {
      reject(o, "paper latency reached the round cutoff");
      return;
    }
    o.wait_started = now;
  }
  bool exchange = s.book_clock != "local";
  auto [view, info] = books.view_at(o.token, o.arrival_ms);
  if (!view && info.why.rfind("the feed has not yet", 0) == 0 &&
      now < o.wait_started + s.book_wait_ms / 1000.0)
    return; // wait for the feed to pass the instant
  if (!view && s.book_quiet && exchange &&
      info.why.rfind("the feed has not yet", 0) == 0) {
    auto quiet = books.view_at(o.token, o.arrival_ms, true);
    view = quiet.first;
    info = quiet.second;
  }
  auto local = books.view_at_mono(o.token, o.arrival_mono);
  Json a = Json::object();
  if (!info.why.empty())
    a["why"] = info.why;
  if (!info.evidence.empty())
    a["evidence"] = info.evidence;
  if (info.book_ts)
    a["book_ts"] = *info.book_ts;
  if (info.applied_wall)
    a["applied_wall"] = *info.applied_wall;
  a["clock"] = s.book_clock;
  a["arrival_ms"] = py::round(o.arrival_ms, 1);
  a["arrival_wall"] = (o.arrival_ms - s.venue_offset_ms) / 1000.0;
  a["arrival_mono"] = o.arrival_mono;
  a["book_wait_ms"] = py::round((now - o.wait_started) * 1000.0, 1);
  a["hwm_lead_ms"] = info.hwm_ms
                         ? Json(py::round(static_cast<double>(*info.hwm_ms) -
                                              o.arrival_ms,
                                          1))
                         : Json(nullptr);
  if (view && info.book_ts)
    a["book_age_ms"] =
        py::round(o.arrival_ms - static_cast<double>(*info.book_ts), 1);
  const std::optional<BookView> &official = exchange ? view : local;
  if (official && official->best_ask())
    a["best_ask"] = *official->best_ask();
  else if (official)
    a["best_ask"] = nullptr;
  std::optional<BookSnapshot> arrived;
  if (!official) {
    if (!a.contains("why"))
      a["why"] = "no book in our hands at that instant";
  } else {
    try {
      arrived = snapshot_from_book_view(*official, o.token, now, h_.venue_wall());
    } catch (const PaperRejected &e) {
      a["why"] = std::string(e.what()).substr(0, 120);
    }
  }
  o.result.arrival = a;
  try {
    if (!arrived)
      throw PaperRejected("unknown book state at the modelled arrival: " +
                          a.value("why", std::string("None")));
    BookSnapshot raw = *arrived;
    BookSnapshot book = net_of_own_fills(raw);
    double matched_wall = a["arrival_wall"].get<double>();
    if (o.fak && book.asks.empty())
      throw PaperRejected("FAK no-fill: no asks on the book at the match");
    if (book.tick_size && o.rules.tick_size &&
        *book.tick_size != *o.rules.tick_size)
      o.rules.tick_size = book.tick_size;
    auto [quiet, held] = check_book(book, o.side, o.token);
    if (!book.best_bid)
      throw PaperRejected("public order book has no bid; spread is unknown");
    Dec spread = book.asks[0].first - *book.best_bid;
    if (spread > D(cfg.max_spread))
      throw PaperRejected("spread " + spread.fixed(6) +
                          " exceeds paper limit " + py::fixed(cfg.max_spread, 6));
    FillQuote q = quote(book, o);
    bool partial =
        o.fak && q.notional < py::quantize(o.spend, MONEY, Rounding::HalfUp);
    if (h_.unix_wall() >= o.end - cfg.min_seconds_to_expiry)
      throw PaperRejected("paper quote reached the round cutoff");
    auto ctx = h_.context();
    if (std::get<0>(ctx) != o.cond || std::get<1>(ctx) != o.up ||
        std::get<2>(ctx) != o.down)
      throw PaperRejected("paper market changed before order booking");
    double cash = h_.cash_balance();
    if (cash + 1e-9 < q.total_cost().to_double())
      throw PaperRejected("paper cash $" + py::fixed(cash, 6) +
                          " is below total cost $" + q.total_cost().fixed(6));
    if (o.plan->hybrid && o.plan->risk_budget_usd) {
      try {
        hybrid::enforce_order_budget(o.spend, o.rules.fee_rate,
                                     D(*o.plan->risk_budget_usd), q.fee);
      } catch (const py::ValueError &e) {
        throw PaperRejected(std::string("hybrid risk check: ") + e.what());
      }
    }
    if (h_.unix_wall() >= o.end - cfg.min_seconds_to_expiry)
      throw PaperRejected("pre-submit guard reached the round cutoff");
    std::string id = h_.record_fill(*o.plan, q);
    if (cfg.own_fill_ttl_s > 0) {
      double m = h_.mono();
      auto &mine = own_[o.token];
      for (auto &[p, sh, nl] : q.levels)
        mine.emplace_back(m, p, sh);
    }
    ++filled_orders;
    Json levels = Json::array();
    for (auto &[p, sh, nl] : q.levels)
      levels.push_back({p.to_double(), sh.to_double(), nl.to_double()});
    Json fill{{"order_id", id},
              {"status", "FILLED"},
              {"side", o.side},
              {"token_id", o.token},
              {"condition_id", o.cond},
              {"requested_amount", q.notional.to_double()}};
    if (partial) {
      fill["partial"] = true;
      fill["stake"] = o.spend.to_double();
    }
    fill["shares"] = q.shares.to_double();
    fill["average_price"] = q.average_price.to_double();
    fill["worst_price"] = q.worst_price.to_double();
    fill["fee"] = q.fee.to_double();
    fill["total_cost"] = q.total_cost().to_double();
    fill["fee_rate"] = o.rules.fee_rate.to_double();
    fill["fee_exponent"] = o.rules.fee_exponent;
    fill["fee_source"] = o.rules.source;
    fill["book_timestamp"] = book.timestamp ? Json(*book.timestamp) : Json();
    fill["book_hash"] = book.book_hash ? Json(*book.book_hash) : Json();
    fill["book_age_ms"] = quiet * 1000.0;
    fill["book_held_ms"] = held * 1000.0;
    fill["assumed_latency_ms"] = o.latency_ms;
    fill["matched_wall"] = matched_wall;
    fill["tag"] = "sniper";
    fill["cash_after"] = h_.cash_balance();
    fill["levels"] = levels;
    fill["wall"] = h_.unix_wall();
    if (h_.audit)
      h_.audit(fill);
    o.result.filled = true;
    o.result.fill = fill;
    o.result.matched_mono = now;
    o.finished = true;
  } catch (const PaperRejected &e) {
    reject(o, e.what());
  }
}

void PaperBroker::poll(
    BookState &books, const SniperSettings &s,
    const std::function<void(const std::shared_ptr<Plan> &,
                             const PaperResult &)> &done) {
  double now = h_.mono();
  for (auto &o : orders_)
    if (!o.finished && now >= o.wake_mono)
      arrive(o, books, s);
  std::vector<Order> finished;
  for (auto it = orders_.begin(); it != orders_.end();) {
    if (it->finished) {
      finished.push_back(std::move(*it));
      it = orders_.erase(it);
    } else
      ++it;
  }
  for (auto &o : finished) {
    if (o.owns_slot)
      busy_.erase(o.token);
    done(o.plan, o.result);
  }
}
} // namespace sniper
