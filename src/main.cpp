#include "sniper/runtime.hpp"
#include "sniper/hybrid.hpp"
#include "sniper/math.hpp"
#include "sniper/strategy.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
namespace sniper {
bool native_self_test() {
  try {
    if (hex(keccak(std::string_view{})) !=
        "0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470")
      return false;
    Signer s(
        "0x0000000000000000000000000000000000000000000000000000000000000001");
    if (!s.self_test() ||
        s.address() != "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf")
      return false;
    if (std::abs(anchored_fair(.5, 1, .0001, 30) - .572434) < 1e-6 &&
        fair_limit(anchored_fair(.5, 1, .0001, 30), .01, .01, .07) == .54 &&
        hybrid::minimum_entry_shares(py::D(.501)) == 10 &&
        hybrid::shares_within_budget(py::D(10), py::D(.54), py::D(.07)) == 17)
      return true;
  } catch (...) {
  }
  return false;
}
int compare_decisions(const std::filesystem::path &a,
                      const std::filesystem::path &b) {
  auto load = [](auto path) {
    std::ifstream file(path);
    if (!file)
      throw std::runtime_error("could not open comparison journal");
    std::vector<Json> rows;
    std::string line;
    while (std::getline(file, line)) {
      auto j = Json::parse(line);
      auto result = j.value("result", "");
      if (!j.contains("side") ||
          (result != "pending" && result != "skipped" && result != "filled" &&
           result != "refused" && result != "error"))
        continue;
      Json row;
      for (auto key :
           {"result", "reason", "side", "cap", "fair_limit", "shares"})
        row[key] = j.contains(key) ? j.at(key) : Json{};
      rows.push_back(row);
    }
    return rows;
  };
  auto x = load(a), y = load(b);
  std::size_t mismatches = 0, total = std::max(x.size(), y.size());
  for (std::size_t i = 0; i < total; ++i)
    if (i >= x.size() || i >= y.size() || x[i] != y[i]) {
      ++mismatches;
      std::cout << Json{{"mismatch", i},
                        {"reference", i < x.size() ? x[i] : Json{}},
                        {"candidate", i < y.size() ? y[i] : Json{}}}
                       .dump()
                << '\n';
    }
  std::cout << Json{{"compared", total},
                    {"mismatches", mismatches},
                    {"match_fraction",
                     total
                         ? Json(static_cast<double>(total - mismatches) / total)
                         : Json{}},
                    {"same_recorded_replay_required", true}}
                   .dump()
            << std::endl;
  return total && mismatches == 0 ? 0 : 1;
}
} // namespace sniper
int main(int argc, char **argv) {
  using namespace sniper;
  std::signal(SIGPIPE, SIG_IGN);
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
      std::cout << "sniper-cpp 1.1.0 native C++20; SDK wire reference 0.13.0\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::cout
          << "sniper-cpp [--paper | --live | --health] [--dash] [--config FILE] "
             "[--run-for SECONDS] [--record FILE]\n  --dash                  "
             "terminal dashboard (DASH_FPS frames a second, default 2)\n  "
             "--dry-run               with --live: every live check, signs but "
             "never sends an order\n  --check                 "
             "validate settings offline\n  --self-test             native "
             "crypto/math check, no network\n  --new-canary-session    "
             "deliberate new live session (backs up old state)\n  --compare A "
             "B           compare decisions from the same recorded "
             "replay\nPaper is the default. --live also requires "
             "SNIPER_LIVE_CANARY=1 and wallet credentials.\n";
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
      auto ok = native_self_test();
      std::cout << (ok ? "Native C++ cryptography and maths self-test passed.\n"
                       : "Native self-test FAILED.\n");
      return ok ? 0 : 1;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--compare")
      return compare_decisions(argv[2], argv[3]);
    if (!native_self_test())
      throw std::runtime_error("native startup self-test failed");
    auto config = Config::load(argc, argv);
    if (config.check) {
      std::cout << config.public_view().dump(2) << '\n';
      return 0;
    }
    Runtime runtime(std::move(config));
    return runtime.run();
  } catch (const std::exception &e) {
    std::cerr << "Startup/runtime refused: " << e.what() << '\n';
    return 1;
  }
}
