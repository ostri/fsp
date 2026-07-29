#pragma once

#include "common.hpp"
#include "error_info.hpp"

#include <cstddef>
#include <expected>
#include <optional>

namespace fsp::ach
{
  /**
   * @brief Stateless IBAN helpers, grouped under a non-instantiable class for discoverability
   * (`ach::utility::check_iban(...)`).
   */
  class utility
  {
  public:
    utility() = delete;

    /**
     * @brief Checks whether a compact-form IBAN (no embedded whitespace, e.g. "SI5601000000100090")
     * is well-formed.
     * @details Validates, in order: the generic ISO 13616 layout (2-letter country code, 2 check
     * digits, 1-30 alphanumeric BBAN characters); the country-specific total length, per
     * https://en.wikipedia.org/wiki/International_Bank_Account_Number#IBAN_formats_by_country
     * (an unrecognized country code fails this step); the ISO 7064 MOD 97-10 checksum shared by
     * every IBAN. For countries whose BBAN carries its own published national check-digit
     * algorithm -- currently only Slovenia (SI) -- that additional check is applied too.
     * @param iban compact-form IBAN, no embedded whitespace
     * @return true if iban passes every check implemented for its country
     */
    [[nodiscard]] static bool check_iban(cstr_t iban);

    /**
     * @brief Expands an IBAN into its human-readable, space-grouped form.
     * @details Strips any existing whitespace and upper-cases the letters, then re-groups the
     * result into blocks of 4 characters separated by a single space. The last block is shorter
     * than 4 whenever the IBAN's length isn't a multiple of 4, and never carries a trailing space.
     * @param iban compact- or already spaced-form IBAN
     * @return the IBAN formatted as e.g. "SI56 0100 0000 0100 090"
     */
    [[nodiscard]] static str_t print_iban(cstr_t iban);

  private:
    [[nodiscard]] static bool                       iso_checksum_ok(cstr_t iban) noexcept;
    [[nodiscard]] static bool                       si_national_check_ok(cstr_t bban) noexcept;
    [[nodiscard]] static std::optional<std::size_t> expected_length(cstr_t country_code) noexcept;
  };

  /**
   * @brief A validated IBAN: a plain string plus a static parse() satisfying
   * fsp::validated_t<>'s contract (see reflection.hpp) -- checks utility::check_iban() at parse
   * time instead of accepting an unchecked string, so a schema field declared
   * fsp::validated_t<ach::iban_t> carries a guaranteed-well-formed IBAN once materialize<T>()
   * succeeds for it.
   */
  struct iban_t
  {
    str_t value; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] static std::expected<iban_t, error_info> parse(cstr_t s);
  };
} // namespace fsp::ach
