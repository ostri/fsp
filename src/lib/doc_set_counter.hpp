// doc_set_counter.hpp
#pragma once
#include "doc_counters.hpp"
#include <vector>
#include <string>
#include <fmt/format.h>

namespace fsp
{
  // Collection of doc_counters, one per document, indexed by doc_ndx -- mirrors the
  // doc_dscr/doc_set_dscr split (one class describing a single document, one class managing
  // the whole set). Size is fixed at construction, since doc_count is already known by the time
  // pipeline::process_files() needs this.
  class doc_set_counter
  {
  public:
    explicit doc_set_counter(std::size_t doc_count);
    [[nodiscard]] doc_counters&       operator[](std::size_t doc_ndx) noexcept;
    [[nodiscard]] const doc_counters& operator[](std::size_t doc_ndx) const noexcept;
    [[nodiscard]] std::size_t         size() const noexcept;
    // Prints every document's dump() nested under a "doc N:" header, each line indented by offs
    // spaces (the per-document dump is indented by offs + 2).
    [[nodiscard]] std::string dump(int offs = 0) const;
  private:
    std::vector<doc_counters> counters_;
  };

  inline doc_set_counter::doc_set_counter(std::size_t doc_count)
  : counters_(doc_count)
  {
  }
  inline doc_counters&       doc_set_counter::operator[](std::size_t doc_ndx) noexcept { return counters_[doc_ndx]; }
  inline const doc_counters& doc_set_counter::operator[](std::size_t doc_ndx) const noexcept { return counters_[doc_ndx]; }
  inline std::size_t         doc_set_counter::size() const noexcept { return counters_.size(); }

  inline std::string doc_set_counter::dump(int offs) const
  {
    auto        leading = std::string(offs, ' ');
    std::string out;
    for (std::size_t i = 0; i < counters_.size(); ++i)
    {
      out += fmt::format("{}doc {}: {}", leading, i, counters_[i].dump());
      if (i + 1 < counters_.size()) out += "\n";
    }
    return out;
  }
} // namespace fsp