// sniper-miss-report: why signals were skipped and orders missed, by reason
// code, from a run's journal. Reads only; sends nothing and needs no keys.
// Works on paper and live journals.
//
// Usage: sniper-miss-report [state/paper/sniper.jsonl] [--record FEED.jsonl]
#include "miss_report.hpp"
#include <exception>
#include <fstream>
#include <iostream>

using namespace sniper::miss;

int main(int argc, char **argv) {
  try {
    std::string journal = "state/paper/sniper.jsonl", record;
    bool named = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--record" && i + 1 < argc)
        record = argv[++i];
      else if (!a.empty() && a[0] != '-' && !named) {
        journal = a;
        named = true;
      } else
        throw std::invalid_argument("usage: sniper-miss-report [JOURNAL] [--record FEED.jsonl]");
    }
    Report report;
    if (!record.empty()) {
      std::ifstream in(record);
      if (!in)
        throw std::runtime_error("cannot open recording: " + record);
      read_lines(in, [&](const Json &e) { report.add_record(e); });
    }
    std::ifstream in(journal);
    if (!in)
      throw std::runtime_error("cannot open journal: " + journal);
    auto bad = read_lines(in, [&](const Json &row) { report.add(row); });
    auto out = report.json();
    out["journal"] = journal;
    out["malformed_lines"] = bad;
    std::cout << out.dump(2) << "\n";
    return bad ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << "sniper-miss-report: " << e.what() << "\n";
    return 1;
  }
}
