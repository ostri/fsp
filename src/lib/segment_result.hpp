#pragma once

// #include "xml_processor.hpp"
#include <fmt/format.h>
#include <string>
// #include <unordered_map>
#include <utility>
#include <vector>
namespace fsp
{
  using cstr_t = std::string_view;
  //  using xpath_result = std::unordered_map<cstr_t, std::vector<std::string>>;
  using xpath_result = std::vector<std::vector<std::string>>;
  struct segment_result
  {
  public:
    segment_result() = default;
    segment_result(std::size_t seg_id, int seg_type);
    segment_result(std::size_t seg_id, int seg_type, xpath_result values);
    [[nodiscard]] std::size_t         seg_id() const;
    [[nodiscard]] int                 seg_type() const;
    [[nodiscard]] const xpath_result& values() const;
    xpath_result&                     values();
    std::string                       dump(int offs = 0);
  private:
    std::size_t  seg_id_   = 0;  // unique id of the segment
    int          seg_type_ = -1; // xpath index that was used to partiion the xml to get this subtree
    xpath_result values_;        // result values
  };
  using vec_seg_result = std::vector<segment_result>;
  //////////////////////////////////////////////////////////////////////
  inline segment_result::segment_result(std::size_t seg_id, int seg_type)
  : seg_id_(seg_id)
  , seg_type_(seg_type)
  {
  }
  inline segment_result::segment_result(std::size_t seg_id, int seg_type, xpath_result values)
  : seg_id_(seg_id)
  , seg_type_(seg_type)
  , values_(std::move(values))
  {
  }
  inline std::size_t         segment_result::seg_id() const { return seg_id_; }
  inline int                 segment_result::seg_type() const { return seg_type_; }
  inline const xpath_result& segment_result::values() const { return values_; }
  inline xpath_result&       segment_result::values() { return values_; }
  //   inline cstr_t              segment_result::values(cstr_t key) const { return values(key, 0); }
  //   inline cstr_t              segment_result::values(std::size_t key, std::size_t ndx) const
  //   {
  // return values_.at(key);
  //   }
} // namespace fsp