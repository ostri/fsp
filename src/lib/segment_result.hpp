#pragma once

// #include "xml_processor.hpp"
#include <fmt/format.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
namespace fsp
{
  using cstr_t       = std::string_view;
  using xpath_result = std::unordered_map<cstr_t, std::vector<std::string>>;
  struct segment_result
  {
  public:
    // segment_result() = default;
    segment_result(std::size_t seg_id, int seg_type)
    : seg_id_(seg_id)
    , seg_type_(seg_type)
    {
    }
    segment_result(std::size_t seg_id, int seg_type, xpath_result values)
    : seg_id_(seg_id)
    , seg_type_(seg_type)
    , values_(std::move(values))
    {
    }
    std::size_t         seg_id() const;
    int                 seg_type() const;
    const xpath_result& values() const;
    xpath_result&       values();
    cstr_t              values(cstr_t key) const { return values(key, 0); }
    cstr_t              values(cstr_t key, std::size_t ndx = 0) const
    {
      const auto it = values_.find(key);
      if (it != values_.end()) return it->second.at(ndx);
      return "";
    }
    std::string dump(int offs = 0);
  private:
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::size_t  seg_id_   = 0;  // unique id of the segment
    int          seg_type_ = -1; // xpath index that was used to partiion the xml to get this subtree
    xpath_result values_;        // result values
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };
} // namespace fsp