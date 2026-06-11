#pragma once

// #include "xml_processor.hpp"
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <vector>
namespace fsp
{
  using cstr_t       = std::string_view;
  using xpath_result = std::unordered_map<cstr_t, std::vector<std::string>>;
  struct segment_result
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::size_t  segment_id  = 0;  // unique id of the segment
    int          xpath_index = -1; // xpath index that was used ti partiion the xml to get this subtree
    xpath_result values;           // result values
    // NOLINTEND(misc-non-private-member-variables-in-classes)
    std::string dump(int offs = 0);
  };
} // namespace fsp