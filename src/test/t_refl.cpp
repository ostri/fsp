#include <cstdint>
#include <meta>
#include <string_view>
#include "work.hpp"

// #include "work.hpp"
#include "fmt/format.h"


int main()
{
  // fsp::print_namespace<^^work>();
  constexpr auto x = fsp::reflex<^^work>();
  static_assert(x.ns() == "work", "namespace name error");
  std::string msg = fmt::format("namespace: {}\n", x.ns());
  fmt::print("{}", msg);
  return 0;
}
