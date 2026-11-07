// sniper-edge-report: calibration and profit from a run's journal.
// Reads only; sends nothing and needs no keys. Works on paper and live journals.
//
// Usage: sniper-edge-report [state/paper/sniper.jsonl] [--ledger PATH]
//                           [--audit PATH] [--min-rounds N]
#include "edge_report.hpp"
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace sniper::edge;
namespace fs = std::filesystem;

template <class F> static int each_line(const fs::path &path, F use) {
  std::ifstream in(path);
  std::string line;
  int bad = 0;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    Json row = Json::parse(line, nullptr, false, true);
    if (row.is_discarded() || !row.is_object()) {
      ++bad; // a crash can leave a cut-off last line
      continue;
    }
    use(row);
  }
  return bad;
}

int main(int argc, char **argv) {
  try {
    fs::path journal = "state/paper/sniper.jsonl", ledger, audit;
    int min_rounds = DEFAULT_MIN_ROUNDS;
    bool named = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      bool more = i + 1 < argc;
      if (a == "--ledger" && more)
        ledger = argv[++i];
      else if (a == "--audit" && more)
        audit = argv[++i];
      else if (a == "--min-rounds" && more)
        min_rounds = std::stoi(argv[++i]);
      else if (!a.empty() && a[0] != '-' && !named) {
        journal = a;
        named = true;
      } else {
        std::cerr << "Usage: sniper-edge-report [journal] [--ledger PATH] [--audit PATH] "
                     "[--min-rounds N]\n";
        return 2;
      }
    }
    if (!fs::is_regular_file(journal)) {
      std::cerr << "No journal at " << journal.string() << "\n";
      return 2;
    }
    if (audit.empty())
      audit = journal.parent_path() / "paper_audit.jsonl";
    if (ledger.empty())
      ledger = journal.parent_path() / "ledger.json";

    Collector collector;
    if (fs::is_regular_file(audit))
      each_line(audit, [&](const Json &row) { collector.add_audit(row); });
    if (fs::is_regular_file(ledger)) {
      std::ifstream in(ledger);
      Json parsed = Json::parse(in, nullptr, false);
      if (parsed.is_object())
        collector.add_ledger(parsed);
      else
        std::cerr << "Ignoring unreadable ledger " << ledger.string() << "\n";
    }
    int bad = each_line(journal, [&](const Json &row) { collector.add(row); });
    std::cout << render(collector.report(min_rounds, bad), journal.string());
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "edge report failed: " << e.what() << "\n";
    return 1;
  }
}
