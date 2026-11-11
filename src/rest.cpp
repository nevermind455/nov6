#include "sniper/rest.hpp"
#include "sniper/pyrt.hpp"
#include <algorithm>
#include <cmath>
namespace sniper::rest {
namespace {
std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}
// An amount in a placement reply: absent, null and "" all mean nothing.
Units reply_amount(const Json &j, const char *key) {
  auto it = j.find(key);
  if (it == j.end() || it->is_null() ||
      (it->is_string() && it->get_ref<const std::string &>().empty()))
    return 0;
  return venue_units(*it);
}
std::string brief(std::string_view text, std::size_t most = 160) {
  return std::string(text.substr(0, most));
}
} // namespace

std::optional<double> move_bp(const std::string &side,
                              std::optional<double> ref_mid,
                              std::optional<double> mid) {
  if (!ref_mid || !mid || !(*ref_mid > 0) || !(*mid > 0) ||
      !std::isfinite(*ref_mid) || !std::isfinite(*mid))
    return std::nullopt;
  double r = (*mid / *ref_mid - 1.0) * 1e4;
  return side == "UP" ? r : -r;
}

std::string direction_refusal(const Json &view) {
  auto dist = view.find("ptb_dist_bp");
  if (dist == view.end() || !dist->is_number()) {
    auto why = view.find("ptb_why");
    return "price to beat unknown" +
           (why != view.end() && why->is_string()
                ? ": " + why->get<std::string>()
                : std::string());
  }
  if (!(dist->get<double>() > 0))
    return "not beating the price to beat";
  auto why = view.find("ptb_why");
  if (why != view.end() && why->is_string() &&
      !why->get_ref<const std::string &>().empty())
    return "beating the price to beat is not certain: " +
           why->get<std::string>();
  return {};
}

double lifetime_s(double end, double wall, const Policy &p) {
  return std::min(p.rest_s, end - wall - p.end_margin_s);
}

std::string refusal(const Missed &m, const Now &now, const Policy &p) {
  if (m.shares <= 0 || m.shares % U || m.limit <= 0 || m.limit >= U)
    return "the missed order is not whole shares at a price";
  if (m.shares < now.min_shares)
    return py::fmt_g(dollars(m.shares)) + " shares is below the venue minimum " +
           py::fmt_g(dollars(now.min_shares)) + " for a resting order";
  double age = now.mono_ms - m.signal_ms;
  if (!std::isfinite(age) || age < 0 || age > p.max_signal_age_ms)
    return "the signal is too old to rest on";
  if (!(lifetime_s(m.end, now.wall, p) >= p.min_rest_s))
    return "too late in the round to rest";
  if (!now.feed_fresh || !now.mid)
    return "no fresh trigger price";
  auto move = move_bp(m.side, m.ref_mid, now.mid);
  if (!move)
    return "the signal's reference price is unknown";
  if (!(*move > 0))
    return "the move was given back";
  if (p.direction) {
    auto why = direction_refusal(now.direction);
    if (!why.empty())
      return why;
  }
  if (now.best_ask && *now.best_ask <= m.limit)
    return "the ask is back inside the limit (a post-only order would cross)";
  return {};
}

std::string cancel_reason(const Order &o, const Now &now, const Policy &p,
                          bool opposite_signal, const std::string &hold) {
  if (!hold.empty())
    return hold;
  if (now.wall >= o.end - p.end_margin_s)
    return "the round's end is near";
  if (now.mono_ms / 1000.0 >= o.deadline_mono)
    return "it rested its time";
  if (opposite_signal)
    return "the trigger fired the other way";
  if (!now.feed_fresh || !now.mid)
    return "no fresh trigger price";
  auto move = move_bp(o.side, o.ref_mid, now.mid);
  if (!move || !(*move > 0))
    return "the move was given back";
  if (p.direction) {
    // Placing needs a clear lead over the price to beat; keeping the order
    // only needs the side not to have fallen behind it.
    auto dist = now.direction.find("ptb_dist_bp");
    if (dist == now.direction.end() || !dist->is_number())
      return "price to beat unknown";
    if (!(dist->get<double>() > 0))
      return "no longer beating the price to beat";
  }
  return {};
}

PostReply parse_post_reply(int http_status, std::string_view body,
                           std::string_view order_id) {
  PostReply out;
  Json j;
  try {
    j = Json::parse(body);
  } catch (const std::exception &) {
    out.detail = "unreadable reply (HTTP " + std::to_string(http_status) + ")";
    return out;
  }
  if (!j.is_object()) {
    out.detail = "unexpected reply (HTTP " + std::to_string(http_status) + ")";
    return out;
  }
  try {
    if (auto s = j.find("status"); s != j.end() && s->is_string())
      out.venue_status = brief(s->get_ref<const std::string &>(), 64);
    if (auto id = j.find("orderID");
        id != j.end() && id->is_string() &&
        !id->get_ref<const std::string &>().empty() && *id != Json(std::string(order_id))) {
      out.detail = "reply order hash disagrees with signed intent";
      return out;
    }
    bool success = j.value("success", false);
    out.making = reply_amount(j, "makingAmount");
    out.taking = reply_amount(j, "takingAmount");
    if (out.making < 0 || out.taking < 0) {
      out = PostReply{};
      out.detail = "negative amounts in the reply";
      return out;
    }
    std::string error = j.value("errorMsg", "");
    if (error.empty() && j.contains("error") && j.at("error").is_string())
      error = j.at("error").get<std::string>();
    std::string status = lower(out.venue_status);
    if (success && (out.taking > 0 || out.making > 0 || status == "matched")) {
      out.kind = Posted::Matched;
      out.detail = "a post-only order matched at placement";
    } else if (success && status == "live" && http_status == 200) {
      out.kind = Posted::Resting;
    } else if (!success && (http_status == 400 || http_status == 200)) {
      std::string text = lower(error);
      bool crossed = text.find("crosses") != std::string::npos;
      out.kind = crossed ? Posted::Crossed : Posted::Refused;
      out.detail = error.empty() ? "venue refused the resting order"
                                 : brief(error);
    } else {
      out.detail = "unexpected reply to a resting order (HTTP " +
                   std::to_string(http_status) +
                   (out.venue_status.empty() ? "" : ", " + out.venue_status) +
                   ")";
    }
  } catch (const std::exception &e) {
    out = PostReply{};
    out.detail = brief(e.what());
  }
  return out;
}

CancelReply parse_cancel_reply(int http_status, std::string_view body,
                               std::string_view order_id) {
  CancelReply out;
  Json j;
  try {
    j = Json::parse(body);
  } catch (const std::exception &) {
    out.detail = "unreadable reply (HTTP " + std::to_string(http_status) + ")";
    return out;
  }
  const std::string id(order_id);
  if (http_status == 200 && j.is_object()) {
    if (auto done = j.find("canceled"); done != j.end() && done->is_array())
      for (auto &v : *done)
        if (v.is_string() && v.get_ref<const std::string &>() == id) {
          out.kind = Cancelled::Yes;
          return out;
        }
    if (auto left = j.find("not_canceled");
        left != j.end() && left->is_object() && left->contains(id)) {
      out.kind = Cancelled::NotOpen;
      auto &why = left->at(id);
      out.detail = brief(why.is_string() ? why.get<std::string>() : why.dump());
      return out;
    }
  }
  out.detail = "HTTP " + std::to_string(http_status) + ": " +
               brief(j.dump(-1, ' ', true), 120);
  return out;
}

bool record_terminal(std::string_view status) {
  std::string s = lower(status);
  return s == "matched" || s == "unmatched" || s == "killed" ||
         s == "expired" || s.rfind("cancel", 0) == 0;
}

bool Book::busy() const {
  return std::any_of(orders_.begin(), orders_.end(),
                     [](const auto &item) { return !item.second.off_book; });
}
bool Book::in_market(std::string_view condition) const {
  return std::any_of(orders_.begin(), orders_.end(), [&](const auto &item) {
    return !item.second.off_book && item.second.condition == condition;
  });
}
Order *Book::find(std::string_view id) {
  auto it = orders_.find(std::string(id));
  return it == orders_.end() ? nullptr : &it->second;
}
void Book::add(Order o) {
  if (o.id.empty())
    throw std::invalid_argument("a resting order needs its id");
  auto id = o.id;
  if (!orders_.emplace(id, std::move(o)).second)
    throw std::runtime_error("resting order already tracked");
}
void Book::posted(std::string_view id) {
  if (auto *o = find(id))
    o->posted = true;
}
void Book::proven(std::string_view id, double mono) {
  if (auto *o = find(id)) {
    if (!o->off_book)
      o->proven_at = mono;
    o->off_book = true;
    o->in_flight = false;
  }
}
bool Book::reopen(std::string_view id) {
  auto *o = find(id);
  if (!o || !o->off_book)
    return false;
  o->off_book = false;
  o->wanted = true;
  o->in_flight = false;
  o->next_cancel = 0;
  return true;
}
bool Book::off(std::string_view id) {
  return orders_.erase(std::string(id)) > 0;
}
bool Book::cancel(std::string_view id, const std::string &why, double mono) {
  auto *o = find(id);
  if (!o)
    return false;
  if (!o->wanted) {
    o->wanted = true;
    o->why = why;
    o->wanted_at = mono;
  }
  return true;
}
std::vector<std::string> Book::due(double mono) {
  std::vector<std::string> out;
  for (auto &[id, o] : orders_) {
    if (!o.wanted || o.off_book)
      continue;
    // A cancel sent before the venue has the order would find nothing, so it
    // waits for the placement's answer - but not for long: an answer that is
    // late leaves the order on the book with nobody watching it.
    if (!o.posted && mono - o.placed_mono < PostGrace)
      continue;
    if (o.in_flight && mono - o.flight_since < FlightTimeout)
      continue;
    if (mono < o.next_cancel)
      continue;
    o.in_flight = true;
    o.flight_since = mono;
    ++o.cancels;
    out.push_back(id);
  }
  return out;
}
void Book::cancel_unproven(std::string_view id, double mono) {
  auto *o = find(id);
  if (!o)
    return;
  o->in_flight = false;
  o->next_cancel = mono + (o->cancels < QuickTries ? Backoff * o->cancels
                                                   : SlowRetry);
}
std::vector<std::string> Book::unproven_to_report(double mono) {
  std::vector<std::string> out;
  for (auto &[id, o] : orders_) {
    if (!o.wanted || o.off_book || o.warned)
      continue;
    bool refused = !o.in_flight && o.cancels >= QuickTries;
    bool stuck = mono - o.wanted_at >= StuckAfter;
    if (refused || stuck) {
      o.warned = true;
      out.push_back(id);
    }
  }
  return out;
}
std::vector<Order> Book::give_up(double wall) {
  std::vector<Order> out;
  for (auto it = orders_.begin(); it != orders_.end();)
    if (wall > it->second.end + GiveUpAfterEnd) {
      out.push_back(std::move(it->second));
      it = orders_.erase(it);
    } else
      ++it;
  return out;
}

bool fits_stop(Units realized, Units open_cost, Units stake, Units stop) {
  if (stop <= 0 || open_cost < 0 || stake < 0)
    return false;
  auto worst = static_cast<__int128>(realized) - open_cost - stake;
  return worst > -static_cast<__int128>(stop);
}

Json Test::json() const {
  return {{"format", "sniper-native-rest-test-1"},
          {"started_wall", started_wall},
          {"off", off},
          {"off_wall", off_wall},
          {"baseline", baseline},
          {"realized", realized},
          {"reason", reason}};
}
Test Test::load(const Json &j) {
  if (!j.is_object() || j.value("format", "") != "sniper-native-rest-test-1")
    throw std::runtime_error("not a resting-test record");
  Test t;
  t.started_wall = j.at("started_wall").get<double>();
  t.off = j.at("off").get<bool>();
  t.off_wall = j.value("off_wall", 0.0);
  t.baseline = j.at("baseline").get<Units>();
  t.realized = j.value("realized", Units(0));
  t.reason = j.value("reason", "");
  if (!std::isfinite(t.started_wall) || t.started_wall <= 0)
    throw std::runtime_error("resting-test record has no start time");
  return t;
}
bool Test::look(Units rest_realized, Units stop, double wall) {
  if (off)
    return false;
  auto change = static_cast<__int128>(rest_realized) - baseline;
  if (change > std::numeric_limits<Units>::max() ||
      change < std::numeric_limits<Units>::min()) {
    off = true;
    off_wall = wall;
    reason = "the resting orders' result is out of range";
    return false;
  }
  realized = static_cast<Units>(change);
  if (stop <= 0 || realized <= -stop) {
    off = true;
    off_wall = wall;
    reason = "the loss stop was reached";
    return false;
  }
  return true;
}
} // namespace sniper::rest
