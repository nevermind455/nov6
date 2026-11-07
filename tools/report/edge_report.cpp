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
  if (!in)
    throw std::runtime_error("cannot open report input: " + path.string());
  return read_json_lines(in, use);
}

int main(int argc, char **argv) {
  try {
    fs::path journal = "state/paper/sniper.jsonl", ledger, audit;
    int min_rounds = DEFAULT_MIN_ROUNDS;
    bool named = false, explicit_ledger = false, explicit_audit = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      bool more = i + 1 < argc;
      if (a == "--ledger" && more) {
        ledger = argv[++i];
        explicit_ledger = true;
      } else if (a == "--audit" && more) {
        audit = argv[++i];
        explicit_audit = true;
      } else if (a == "--min-rounds" && more)
        min_rounds = parse_min_rounds(argv[++i]);
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
    if ((explicit_audit && audit.empty()) || (explicit_ledger && ledger.empty()))
      throw std::invalid_argument("explicit audit/ledger paths must not be empty");
    if (audit.empty())
      audit = journal.parent_path() / "paper_audit.jsonl";
    if (ledger.empty())
      ledger = journal.parent_path() / "ledger.json";
    if ((explicit_audit && !fs::is_regular_file(audit)) ||
        (explicit_ledger && !fs::is_regular_file(ledger))) {
      std::cerr << "Explicit audit/ledger input is missing or is not a regular file\n";
      return 2;
    }

    Collector collector;
    int bad = 0;
    if (fs::is_regular_file(audit))
      bad += each_line(audit, [&](const Json &row) { collector.add_audit(row); });
    if (fs::is_regular_file(ledger)) {
      std::ifstream in(ledger);
      if (!in)
        throw std::runtime_error("cannot open ledger: " + ledger.string());
      Json parsed = Json::parse(in, nullptr, false);
      if (in.bad())
        throw std::runtime_error("error reading ledger: " + ledger.string());
      if (parsed.is_object())
        collector.add_ledger(parsed);
      else
        throw std::runtime_error("invalid ledger: " + ledger.string());
    }
    bad += each_line(journal, [&](const Json &row) { collector.add(row); });
    auto report = collector.report(min_rounds, bad);
    std::cout << render(report, journal.string());
    return report.bad_lines || report.invalid_rows || report.untraceable ? 1 : 0;
  } catch (const std::invalid_argument &e) {
    std::cerr << "edge report argument failed: " << e.what() << "\n";
    return 2;
  } catch (const std::exception &e) {
    std::cerr << "edge report failed: " << e.what() << "\n";
    return 1;
  }
}
