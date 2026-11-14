#include "sniper/feeds.hpp"
#include <cmath>

namespace sniper::feeds {
namespace {
// A decimal string or number, finite and positive.
std::optional<double> positive(const Json &v) {
  double x;
  if (v.is_number())
    x = v.get<double>();
  else if (v.is_string()) {
    const auto &s = v.get_ref<const std::string &>();
    char *end = nullptr;
    x = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size())
      return std::nullopt;
  } else
    return std::nullopt;
  if (!std::isfinite(x) || x <= 0)
    return std::nullopt;
  return x;
}
std::optional<double> stamp(const Json &v) {
  if (!v.is_number() || v.is_boolean())
    return std::nullopt;
  double x = v.get<double>();
  return std::isfinite(x) && x > 0 ? std::optional(x) : std::nullopt;
}
Tick book(const Json &d, bool with_time) {
  Tick t;
  auto symbol = d.find("s");
  if (symbol == d.end() || *symbol != "BTCUSDT")
    return t; // another symbol: not ours
  auto bid = d.contains("b") ? positive(d["b"]) : std::nullopt;
  auto ask = d.contains("a") ? positive(d["a"]) : std::nullopt;
  t.kind = Tick::Malformed;
  if (!bid || !ask || *bid > *ask)
    return t;
  t.kind = Tick::Book;
  t.mid = (*bid + *ask) / 2.0;
  if (with_time && d.contains("E"))
    t.event_ms = stamp(d["E"]);
  return t;
}
} // namespace

bool known(std::string_view feed) {
  return feed == "binance_fut" || feed == "binance_spot";
}

std::string url(std::string_view feed) {
  if (feed == "binance_fut")
    return "wss://fstream.binance.com/ws/btcusdt@bookTicker";
  if (feed == "binance_spot")
    return "wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade";
  throw std::invalid_argument("unknown trigger feed " + std::string(feed));
}

Tick parse(std::string_view feed, std::string_view raw) {
  Json d;
  try {
    d = Json::parse(raw);
  } catch (const std::exception &) {
    return Tick{Tick::Malformed, {}, {}};
  }
  if (!d.is_object())
    return Tick{Tick::Malformed, {}, {}};
  if (feed == "binance_fut") {
    if (d.value("e", "") != "bookTicker")
      return Tick{}; // a subscription answer
    return book(d, true);
  }
  if (feed == "binance_spot") {
    // the combined stream: {"stream": "btcusdt@bookTicker" | "btcusdt@trade", "data": {...}}
    auto stream = d.find("stream");
    auto data = d.find("data");
    if (stream == d.end() || data == d.end() || !data->is_object())
      return Tick{};
    if (*stream == "btcusdt@bookTicker")
      return book(*data, false); // no exchange time on spot book tickers
    if (*stream == "btcusdt@trade") {
      Tick t;
      if (data->value("s", "") != "BTCUSDT")
        return t;
      t.kind = Tick::Trade;
      t.event_ms = data->contains("E") ? stamp((*data)["E"]) : std::nullopt;
      return t;
    }
    return Tick{};
  }
  return Tick{Tick::Malformed, {}, {}};
}
} // namespace sniper::feeds
