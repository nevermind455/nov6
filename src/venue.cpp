#include "sniper/venue.hpp"
#include "sniper/storage.hpp"
#include <algorithm>
namespace sniper {
bool ServerClock::sample(double server, double sent, double received,
                         double rtt, double mono) {
  if (!std::isfinite(server) || !std::isfinite(sent) ||
      !std::isfinite(received) || !std::isfinite(rtt) || server <= 0 ||
      rtt < 0 || rtt > 500 || received < sent ||
      std::abs((received - sent) * 1000 - rtt) > 1000)
    return false;
  double mid = server + (server == std::floor(server) ? .5 : 0);
  offsets.push_back(mid - (sent + received) / 2);
  if (offsets.size() > 5)
    offsets.pop_front();
  auto sorted = std::vector<double>(offsets.begin(), offsets.end());
  std::sort(sorted.begin(), sorted.end());
  offset =
      sorted.size() % 2
          ? sorted[sorted.size() / 2]
          : (sorted[sorted.size() / 2 - 1] + sorted[sorted.size() / 2]) / 2;
  at = mono;
  server_at = received + offset;
  rtts.push_back(rtt);
  if (rtts.size() > 50)
    rtts.pop_front();
  return true;
}
std::int64_t ServerClock::now(double mono) const {
  if (at < 0)
    throw std::runtime_error("server clock never synced");
  if (mono - at < 0 || mono - at > 60)
    throw std::runtime_error("server clock stale");
  return static_cast<std::int64_t>(server_at + mono - at);
}
double ServerClock::median_rtt() const {
  if (rtts.empty())
    return 14;
  auto v = std::vector<double>(rtts.begin(), rtts.end());
  std::sort(v.begin(), v.end());
  return v.size() % 2 ? v[v.size() / 2]
                      : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2;
}
Credentials derive_credentials(const RestClient &rest, const Signer &signer) {
  auto time = rest.get("https://clob.polymarket.com/time");
  auto stamp = static_cast<std::int64_t>(number(time));
  auto signature = signer.sign(auth_digest(signer.address(), stamp));
  Headers headers{{"POLY_ADDRESS", signer.address()},
                  {"POLY_NONCE", "0"},
                  {"POLY_SIGNATURE", signature},
                  {"POLY_TIMESTAMP", std::to_string(stamp)}};
  auto response = rest.request(
      "GET", "https://clob.polymarket.com/auth/derive-api-key", headers);
  if (response.status == 400 || response.status == 404)
    response = rest.request("POST", "https://clob.polymarket.com/auth/api-key",
                            headers);
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error(
        "native API credential derivation refused by CLOB");
  auto j = response.json();
  Credentials c{j.at("apiKey"), j.at("secret"), j.at("passphrase")};
  if (!c.valid())
    throw std::runtime_error("incomplete CLOB API credentials");
  return c;
}
static std::string call_data(std::string_view signature,
                             std::initializer_list<Hash> words) {
  auto hash = keccak(signature);
  std::string out = hex(std::span(hash.data(), 4));
  for (auto &w : words)
    out += hex(w, false);
  return out;
}
static Hash bytes32(std::string_view s) {
  auto b = unhex(s);
  if (b.size() != 32)
    throw std::runtime_error("invalid condition id");
  Hash h;
  std::copy(b.begin(), b.end(), h.begin());
  return h;
}
std::map<std::string, Units> resolve_payouts(const RestClient &rest,
                                             const Round &r,
                                             std::string_view rpc,
                                             bool onchain) {
  if (wall_s() < r.end + 55)
    return {};
  if (onchain) {
    try {
      auto cid = bytes32(r.condition);
      Hash zero{};
      std::vector<std::string> calls{
          call_data("payoutDenominator(bytes32)", {cid}),
          call_data("payoutNumerators(bytes32,uint256)", {cid, word(0)}),
          call_data("payoutNumerators(bytes32,uint256)", {cid, word(1)}),
          call_data("getCollectionId(bytes32,bytes32,uint256)",
                    {zero, cid, word(1)}),
          call_data("getCollectionId(bytes32,bytes32,uint256)",
                    {zero, cid, word(2)})};
      Json batch = Json::array();
      for (std::size_t n = 0; n < calls.size(); ++n)
        batch.push_back(
            {{"jsonrpc", "2.0"},
             {"id", n + 1},
             {"method", "eth_call"},
             {"params",
              Json::array({Json{{"to", CTF}, {"data", calls[n]}}, "latest"})}});
      auto response = rest.request("POST", rpc, {}, batch.dump(), 8);
      if (response.status != 200)
        throw std::runtime_error("settlement RPC failed");
      auto replies = response.json();
      std::array<std::string, 5> data;
      for (auto &v : replies) {
        auto id = v.at("id").get<int>();
        if (id < 1 || id > 5 || !v.contains("result"))
          throw std::runtime_error("invalid settlement batch reply");
        data[id - 1] = v.at("result");
      }
      Big denominator(data[0]), n0(data[1]), n1(data[2]);
      if (denominator <= 0)
        return {};
      if (n0 < 0 || n1 < 0 || n0 + n1 != denominator)
        throw std::runtime_error("invalid payout vector");
      if (!((n0 == 0 || n0 == denominator) ||
            (n0 * 2 == denominator && n1 * 2 == denominator)))
        throw std::runtime_error("unsupported payout split");
      for (auto collateral :
           {std::string("0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"),
            std::string(COLLATERAL)}) {
        std::array<std::string, 2> ids;
        for (int i = 0; i < 2; ++i) {
          auto coll = unhex(collateral), collection = unhex(data[3 + i]);
          if (collection.size() != 32)
            throw std::runtime_error("invalid collection id");
          coll.insert(coll.end(), collection.begin(), collection.end());
          auto h = keccak(coll);
          Big id = 0;
          for (auto byte : h)
            id = (id << 8) + byte;
          ids[i] = id.convert_to<std::string>();
        }
        if ((ids[0] == r.up && ids[1] == r.down) ||
            (ids[0] == r.down && ids[1] == r.up))
          return {{ids[0], (n0 * U / denominator).convert_to<Units>()},
                  {ids[1], (n1 * U / denominator).convert_to<Units>()}};
      }
      throw std::runtime_error("on-chain outcome-token mapping not proven");
    } catch (...) {
      if (wall_s() < r.end + 300)
        return {};
    }
  }
  if (wall_s() < r.end + 300)
    return {};
  try {
    auto m = rest.get("https://clob.polymarket.com/markets/" + r.condition);
    if (!m.value("closed", false) || m.value("condition_id", "") != r.condition)
      return {};
    std::map<std::string, Units> result;
    int wins = 0;
    for (auto &t : m.at("tokens")) {
      std::string id = t.at("token_id");
      if (id != r.up && id != r.down)
        continue;
      bool winner = t.at("winner").get<bool>();
      result[id] = winner ? U : 0;
      wins += winner;
    }
    if (result.size() == 2 && wins == 1)
      return result;
  } catch (...) {
  }
  try {
    auto e = rest.get("https://gamma-api.polymarket.com/events/slug/" + r.slug);
    for (auto &m : e.at("markets")) {
      if (m.value("conditionId", "") != r.condition ||
          !m.value("closed", false) ||
          m.value("umaResolutionStatus", "") != "resolved")
        continue;
      auto tokens = m.at("clobTokenIds"), prices = m.at("outcomePrices");
      if (tokens.is_string())
        tokens = Json::parse(tokens.get<std::string>());
      if (prices.is_string())
        prices = Json::parse(prices.get<std::string>());
      if (tokens.size() != 2 || prices.size() != 2)
        continue;
      std::map<std::string, Units> out;
      Units sum = 0;
      for (int i = 0; i < 2; ++i) {
        auto id = tokens[i].get<std::string>();
        auto p = as_units(prices[i]);
        if ((id != r.up && id != r.down) || (p != 0 && p != U && p != U / 2))
          throw std::runtime_error("invalid mirror payout");
        out[id] = p;
        sum += p;
      }
      if (out.size() == 2 && sum == U)
        return out;
    }
  } catch (...) {
  }
  return {};
}
ParsedTrade parse_trade(const Json &j,
                        const std::map<std::string, Intent> &intents) {
  ParsedTrade t;
  t.id = j.at("id");
  t.order = j.at("taker_order_id");
  t.token = j.at("asset_id");
  t.condition = j.at("market");
  t.status = j.at("status");
  auto it = intents.find(t.order);
  if (it == intents.end())
    throw std::runtime_error("fill the native bot did not authorize");
  auto &i = it->second;
  if (i.token != t.token || i.condition != t.condition || j.at("side") != "BUY")
    throw std::runtime_error("fill identity disagrees with authorized order");
  t.shares = as_units(j.at("size"));
  auto price = as_units(j.at("price"));
  if (t.shares <= 0 || price <= 0 || price >= U)
    throw std::runtime_error("invalid private fill");
  t.cost = multiply(t.shares, price);
  t.fee = fee_total(t.shares, price, i.fee_rate, i.fee_exponent);
  if (j.contains("maker_orders") && j.at("maker_orders").is_array() &&
      !j.at("maker_orders").empty()) {
    Units size = 0, cost = 0, fees = 0;
    for (auto &leg : j.at("maker_orders")) {
      auto p = as_units(leg.at("price")),
           sh = as_units(leg.at("matched_amount"));
      if (leg.at("asset_id") != t.token) {
        auto opposite = i.up == t.token ? i.down : i.up;
        if (opposite.empty() || leg.at("asset_id") != opposite)
          throw std::runtime_error(
              "maker leg token is outside this binary market");
        p = U - p;
      }
      if (sh <= 0 || p <= 0 || p >= U)
        throw std::runtime_error("invalid maker leg");
      size += sh;
      cost += multiply(sh, p);
      fees += fee_total(sh, p, i.fee_rate, i.fee_exponent);
    }
    if (std::abs(size - t.shares) > 1)
      throw std::runtime_error("maker legs do not match trade size");
    t.cost = cost;
    t.fee = fees;
  }
  return t;
}
} // namespace sniper
