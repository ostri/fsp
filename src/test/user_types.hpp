#pragma once

// Illustrative "user-defined" validated field type -- as if written by a downstream programmer
// using fsp, entirely outside the fsp library itself. See fsp::ach::iban_t (ach/utility.hpp) for
// a library-provided validated type using the same fsp::validated_t<>/parse() contract.

#include "error_info.hpp"
#include "parsing_util.hpp"
#include "value_set_t.hpp"

#include <expected>
#include <fmt/format.h>

namespace usr
{
  /**
   * @brief A validated BIC (agent) code: must appear in a reference table loaded once via
   * bic_code_t::init() -- see fsp::value_set_t's own class comment. Distinct from
   * fsp::value_set_t<some_other_tag> instantiations (e.g. a currency-code set) via bic_codes_tag,
   * which exists purely to give this alias its own, separately-initialized static storage.
   */
  struct bic_codes_tag
  {
  };
  using bic_code_t = fsp::value_set_t<bic_codes_tag>;


  /**
   * @brief A validated fsp::amount_t bounded to [Min, Max], both given in whole currency units
   * (e.g. bounded_amount_t<1, 50000> accepts 1.00000 .. 50000.00000).
   * @details fsp::amount_t stores its value scaled by 10^fsp::amount_scale (see parsing_util.hpp
   * -- e.g. "1.00000" is the raw integer 100000), so Min/Max are multiplied by
   * fsp::amount_scale_factor before comparing against the parsed value's raw, scaled integer --
   * the schema declaration itself stays in human units.
   * @tparam Min inclusive lower bound, in whole currency units
   * @tparam Max inclusive upper bound, in whole currency units
   */
  template <fsp::big_int_t Min, fsp::big_int_t Max>
  struct bounded_amount_t
  {
    fsp::amount_t value; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] static std::expected<bounded_amount_t, fsp::error_info> parse(fsp::cstr_t s)
    {
      const fsp::amount_t   a          = fsp::parse_iso_amount(s);
      const fsp::big_int_t min_scaled = Min * fsp::amount_scale_factor;
      const fsp::big_int_t max_scaled = Max * fsp::amount_scale_factor;
      if (a.value < min_scaled || a.value > max_scaled)
        return std::unexpected(
          fsp::error_info::semantic("amount_out_of_range", fmt::format("amount {} outside allowed range [{}, {}]", a, Min, Max)));
      return bounded_amount_t{a};
    }
  };
} // namespace usr
