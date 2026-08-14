// value_set_t.hpp
#pragma once
#include "error_info.hpp"
#include <expected>
#include <fmt/format.h>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>

namespace fsp
{
  using str_t  = std::string;
  using cstr_t = std::string_view;

  /**
   * @brief A validated_t<> field type (see reflection.hpp) that checks membership in a fixed set
   * of allowed values, loaded once from a caller-supplied delimited string_view (see init())
   * instead of hand-writing one struct per set. Covers both "must equal exactly one fixed value"
   * (e.g. a mandatory "SEPA" scheme code -- init() with a single-element set) and "must be one of
   * many" (e.g. a BIC reference table) with the same type.
   * @details init() populates a Tag-specific table exactly once, ahead of the run (typically from
   * your own pipeline_hooks::on_run_start() override, or from main() before exec() -- see init()'s
   * own doc comment). Every parse() call after that -- there can be millions, one per matching
   * field across every worker thread -- only ever does a lookup, never touches the caller's raw
   * data again. table() itself stores string_views, not owning strings, into the caller's own
   * packed_values buffer -- see init()'s own doc comment for the lifetime this implies.
   * @tparam Tag any type used purely to force a distinct template instantiation (and thus
   * distinct static storage in table()) per logical set -- e.g. an empty tag struct declared
   * next to the alias that names it:
   *
   *   struct bic_codes_tag {};
   *   using bic_code_t = fsp::value_set_t<bic_codes_tag>;
   *
   * Never constructed, never referenced by value -- it only ever appears as a template argument.
   */
  template <typename Tag>
  class value_set_t
  {
  public:
    str_t value; // NOLINT(misc-non-private-member-variables-in-classes)

    /**
     * @brief Populates this Tag's allowed-value set from a caller-owned, delimiter-separated
     * string_view. Must be called exactly once, on the main thread, strictly before any worker
     * thread starts processing segments -- table() below is a plain (non-atomic, non-locked)
     * static, since every parse() call after init() only ever reads it.
     * @param packed_values the caller's own buffer, e.g. "AAAADEBBXXX;BBBBFRPPXXX" -- NOT copied:
     *   table() stores string_views into it, so it must stay alive and unmodified for as long as
     *   any parse() call for this Tag can still happen, i.e. for the whole run.
     * @param delimiter single-character separator between values (e.g. ';'); a single value with
     *   no delimiter occurrences at all is a valid one-element set (see the class's own comment
     *   on the "must equal exactly one fixed value" case).
     */
    static void init(cstr_t packed_values, char delimiter)
    {
      auto& t = table();
      t.clear();
      for (const auto part : std::views::split(packed_values, delimiter)) t.emplace(part.data(), part.size());
    }

    /** @brief validated_t<>'s required contract (see reflection.hpp) -- true iff s is in the set init() populated. */
    [[nodiscard]] static std::expected<value_set_t, error_info> parse(cstr_t s)
    {
      if (! table().contains(s)) return std::unexpected(error_info::semantic("value_not_in_set", fmt::format("'{}' not in allowed set", s)));
      return value_set_t{str_t(s)};
    }
  private:
    // One instance per Tag (distinct template instantiation -> distinct static storage) -- see
    // the class's own tparam doc comment. Views, not owning strings: init()'s own doc comment
    // covers the lifetime requirement this implies for the caller's packed_values buffer.
    [[nodiscard]] static std::unordered_set<cstr_t>& table()
    {
      static std::unordered_set<cstr_t> t;
      return t;
    }
  };
} // namespace fsp
