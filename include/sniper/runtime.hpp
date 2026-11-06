#pragma once
#include "config.hpp"
#include <memory>
namespace sniper {
class Runtime {
  struct Impl;
  std::unique_ptr<Impl> p_;

public:
  explicit Runtime(Config);
  ~Runtime();
  int run();
};
bool native_self_test();
int compare_decisions(const std::filesystem::path &,
                      const std::filesystem::path &);
} // namespace sniper
