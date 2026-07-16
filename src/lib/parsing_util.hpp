#pragma once

#include "xml_attr.hpp"
#include "xpath_set.hpp"

#include <cassert>
#include <chrono>
#include <fmt/format.h>
// #include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fsp
{
  using date_t = std::chrono::year_month_day;
  using cstr_t = std::string_view;

  // --- main structure -----------------------------------------------------------------


  // --- proc_data ----------------------------------------------------------------------
  // Opomba: xpaths ostane std::vector — proc_data ni constexpr, je runtime struktura.
  // Če bi hoteli constexpr proc_data, bi potrebovali std::array<xpath_node_struct, N>.
  struct proc_data
  {
    fsp::xpath_set              targets; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<fsp::xpath_set> xpaths;  // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] std::string dump(int offs = 0) const
    { return fmt::format("{0}targets:{1}\n{0}xpaths.size:{2}", std::string(offs, ' '), targets.dump(offs), xpaths.size()); }
  };

  // --- build --------------------------------------------------------------------------
  [[nodiscard]] constexpr xpath_set build(std::span<const raw_attr> raw_paths, std::span<const ns> ns_arr) { return {raw_paths, ns_arr}; }


} // namespace fsp
