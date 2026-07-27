#pragma once

#include "result_values.hpp"
#include <fmt/format.h>
#include <string>
#include <utility>
#include <vector>
namespace fsp
{
  using cstr_t = std::string_view;
  struct segment_result
  {
  public:
    segment_result() = default;
    segment_result(std::size_t seg_id, int seg_type, int doc_ndx);
    segment_result(std::size_t seg_id, int seg_type, result_values values, int doc_ndx);
    [[nodiscard]] std::size_t          seg_id() const;
    [[nodiscard]] int                  seg_type() const;
    [[nodiscard]] const result_values& values() const;
    result_values&                     values();
    std::string                        dump(int offs = 0);
    [[nodiscard]] int                  doc_ndx() const;
  private:
    std::size_t   seg_id_   = 0;  // unique id of the segment
    int           seg_type_ = -1; // xpath index that was used to partiion the xml to get this subtree
    int           doc_ndx_  = -1; // document id where this segment_result occurred
    result_values values_;        // result values
  };
  using vec_seg_result = std::vector<segment_result>;
  //////////////////////////////////////////////////////////////////////
  inline segment_result::segment_result(std::size_t seg_id, int seg_type, int doc_ndx)
  : seg_id_(seg_id)
  , seg_type_(seg_type)
  , doc_ndx_(doc_ndx)
  {
  }
  inline segment_result::segment_result(std::size_t seg_id, int seg_type, result_values values, int doc_ndx)
  : seg_id_(seg_id)
  , seg_type_(seg_type)
  , doc_ndx_(doc_ndx)
  , values_(std::move(values))
  {
  }
  inline std::size_t          segment_result::seg_id() const { return seg_id_; }
  inline int                  segment_result::seg_type() const { return seg_type_; }
  inline int                  segment_result::doc_ndx() const { return doc_ndx_; }
  inline const result_values& segment_result::values() const { return values_; }
  inline result_values&       segment_result::values() { return values_; }
} // namespace fsp