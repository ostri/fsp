// doc_set_counter.hpp
#pragma once
#include "doc_counters.hpp"
#include "doc_set_dscr.hpp"
#include <vector>
#include <string>
#include <fmt/format.h>

namespace fsp
{
  using str_t = std::string;
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
    [[nodiscard]] str_t               dump(int offs = 0) const;

    // Whole-run totals, each computed by summing/counting across all documents once (not
    // maintained as a separate running counter -- see doc_counters.hpp for why). ds_dscr is the
    // SAME doc_set_dscr the run itself used (e.g. importer/pipeline's own ds_dscr()) -- syntax/
    // validation outcome now lives entirely in doc_dscr::status() (doc_status_t, see doc_dscr.hpp),
    // not here: doc_counters only ever tracked segment-processing facts, never syntax/validation.
    [[nodiscard]] std::size_t total_segments(const doc_set_dscr& ds_dscr) const noexcept; // sum of total() across all documents
    [[nodiscard]] std::size_t total_docs() const noexcept;                                // number of documents in this run (== size())
    // "Syntactically correct/incorrect" = did cutting finish successfully (cut_finished()) AND
    // doc_dscr::status() reports both syntax and validation as three_state::valid. Either a
    // failed cut (parse/XSD error, or already known invalid before cutting started) or a separate
    // V failure counts as syntactically incorrect.
    [[nodiscard]] std::size_t syntactically_correct_docs(const doc_set_dscr& ds_dscr) const noexcept;
    [[nodiscard]] std::size_t syntactically_incorrect_docs(const doc_set_dscr& ds_dscr) const noexcept;
    // "Semantically correct/incorrect" only makes sense for documents that are syntactically
    // correct (see above) -- correct means every one of its segments passed the
    // on_semantic_check verdict (error() == 0), incorrect means at least one did not. A
    // syntactically incorrect document counts toward neither (semantically_correct_docs() +
    // semantically_incorrect_docs() == syntactically_correct_docs()).
    [[nodiscard]] std::size_t semantically_correct_docs(const doc_set_dscr& ds_dscr) const noexcept;
    [[nodiscard]] std::size_t semantically_incorrect_docs(const doc_set_dscr& ds_dscr) const noexcept;
    [[nodiscard]] std::size_t total_segments_ok(const doc_set_dscr& ds_dscr) const noexcept;
    [[nodiscard]] std::size_t total_segments_error(const doc_set_dscr& ds_dscr) const noexcept;
  private:
    // True iff doc_ndx's cut finished AND doc_dscr::status() reports both syntax and validation
    // as three_state::valid -- the shared "syntactically correct" predicate every method above uses.
    [[nodiscard]] bool        syntactically_correct(std::size_t doc_ndx, const doc_set_dscr& ds_dscr) const noexcept;
    std::vector<doc_counters> counters_;
  };

  inline doc_set_counter::doc_set_counter(std::size_t doc_count)
  : counters_(doc_count)
  {
  }
  inline doc_counters&       doc_set_counter::operator[](std::size_t doc_ndx) noexcept { return counters_[doc_ndx]; }
  inline const doc_counters& doc_set_counter::operator[](std::size_t doc_ndx) const noexcept { return counters_[doc_ndx]; }
  inline std::size_t         doc_set_counter::size() const noexcept { return counters_.size(); }

  inline bool doc_set_counter::syntactically_correct(std::size_t doc_ndx, const doc_set_dscr& ds_dscr) const noexcept
  {
    const auto& c = counters_[doc_ndx]; // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- caller-bounded loop
    if (! c.cut_finished()) return false;
    const auto& st =
      ds_dscr[doc_ndx].status(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- caller-bounded loop
    return st.syntax_status() == three_state::valid && st.valid_status() == three_state::valid;
  }

  inline std::size_t doc_set_counter::total_segments(const doc_set_dscr& ds_dscr) const noexcept
  {
    // Only counts segments belonging to a syntactically correct document -- a document that
    // never finished cutting (or failed a separate V pass) may have processed some segments
    // before it was abandoned, and those don't belong in a whole-run total.
    std::size_t sum = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr))
        sum += counters_[i].total(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return sum;
  }
  inline std::size_t doc_set_counter::total_segments_ok(const doc_set_dscr& ds_dscr) const noexcept
  {
    std::size_t sum = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr))
        sum += counters_[i].ok(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return sum;
  }
  inline std::size_t doc_set_counter::total_segments_error(const doc_set_dscr& ds_dscr) const noexcept
  {
    std::size_t sum = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr))
        sum += counters_[i].error(); // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return sum;
  }

  inline std::size_t doc_set_counter::total_docs() const noexcept { return counters_.size(); }

  inline std::size_t doc_set_counter::syntactically_correct_docs(const doc_set_dscr& ds_dscr) const noexcept
  {
    std::size_t count = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr)) ++count;
    return count;
  }
  inline std::size_t doc_set_counter::syntactically_incorrect_docs(const doc_set_dscr& ds_dscr) const noexcept
  { return total_docs() - syntactically_correct_docs(ds_dscr); }

  inline std::size_t doc_set_counter::semantically_correct_docs(const doc_set_dscr& ds_dscr) const noexcept
  {
    std::size_t count = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr) &&
          counters_[i].error() == 0) // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ++count;
    return count;
  }
  inline std::size_t doc_set_counter::semantically_incorrect_docs(const doc_set_dscr& ds_dscr) const noexcept
  {
    std::size_t count = 0;
    for (std::size_t i = 0; i < counters_.size(); ++i)
      if (syntactically_correct(i, ds_dscr) &&
          counters_[i].error() > 0) // NOLINT(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        ++count;
    return count;
  }

  inline str_t doc_set_counter::dump(int offs) const
  {
    auto  leading = str_t(offs, ' ');
    str_t out;
    for (std::size_t i = 0; i < counters_.size(); ++i)
    {
      out += fmt::format("{}doc {}: {}", leading, i, counters_[i].dump());
      if (i + 1 < counters_.size()) out += "\n";
    }
    return out;
  }
} // namespace fsp