#include "utility.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fmt/format.h>

namespace
{
  struct country_len
  {
    fsp::cstr_t  code;
    std::uint8_t length;
  };

  // IBAN length by country, per the SWIFT IBAN registry (see
  // https://en.wikipedia.org/wiki/International_Bank_Account_Number#IBAN_formats_by_country).
  // Countries that merely use an "IBAN-like" number without being part of the official registry
  // are intentionally left out -- check_iban() rejects an unrecognized country code.
  // clang-format off
  // NOLINTBEGIN(readability-magic-numbers)
  constexpr auto country_lengths = std::to_array<country_len>({
    {.code = "AD", .length = 24}, {.code = "AE", .length = 23}, {.code = "AL", .length = 28}, {.code = "AT", .length = 20},
    {.code = "AZ", .length = 28}, {.code = "BA", .length = 20}, {.code = "BE", .length = 16}, {.code = "BG", .length = 22},
    {.code = "BH", .length = 22}, {.code = "BI", .length = 27}, {.code = "BR", .length = 29}, {.code = "BY", .length = 28},
    {.code = "CH", .length = 21}, {.code = "CR", .length = 22}, {.code = "CY", .length = 28}, {.code = "CZ", .length = 24},
    {.code = "DE", .length = 22}, {.code = "DJ", .length = 27}, {.code = "DK", .length = 18}, {.code = "DO", .length = 28},
    {.code = "EE", .length = 20}, {.code = "EG", .length = 29}, {.code = "ES", .length = 24}, {.code = "FI", .length = 18},
    {.code = "FO", .length = 18}, {.code = "FR", .length = 27}, {.code = "GB", .length = 22}, {.code = "GE", .length = 22},
    {.code = "GI", .length = 23}, {.code = "GL", .length = 18}, {.code = "GR", .length = 27}, {.code = "GT", .length = 28},
    {.code = "HR", .length = 21}, {.code = "HU", .length = 28}, {.code = "IE", .length = 22}, {.code = "IL", .length = 23},
    {.code = "IQ", .length = 23}, {.code = "IS", .length = 26}, {.code = "IT", .length = 27}, {.code = "JO", .length = 30},
    {.code = "KM", .length = 27}, {.code = "KW", .length = 30}, {.code = "KZ", .length = 20}, {.code = "LB", .length = 28},
    {.code = "LC", .length = 32}, {.code = "LI", .length = 21}, {.code = "LT", .length = 20}, {.code = "LU", .length = 20},
    {.code = "LV", .length = 21}, {.code = "LY", .length = 25}, {.code = "MC", .length = 27}, {.code = "MD", .length = 24},
    {.code = "ME", .length = 22}, {.code = "MK", .length = 19}, {.code = "MR", .length = 27}, {.code = "MT", .length = 31},
    {.code = "MU", .length = 30}, {.code = "NL", .length = 18}, {.code = "NO", .length = 15}, {.code = "OM", .length = 23},
    {.code = "PK", .length = 24}, {.code = "PL", .length = 28}, {.code = "PS", .length = 29}, {.code = "PT", .length = 25},
    {.code = "QA", .length = 29}, {.code = "RO", .length = 24}, {.code = "RS", .length = 22}, {.code = "RU", .length = 33},
    {.code = "SA", .length = 24}, {.code = "SC", .length = 31}, {.code = "SD", .length = 18}, {.code = "SE", .length = 24},
    {.code = "SI", .length = 19}, {.code = "SK", .length = 24}, {.code = "SM", .length = 27}, {.code = "SO", .length = 23},
    {.code = "ST", .length = 25}, {.code = "SV", .length = 28}, {.code = "TL", .length = 23}, {.code = "TN", .length = 24},
    {.code = "TR", .length = 26}, {.code = "UA", .length = 29}, {.code = "VA", .length = 22}, {.code = "VG", .length = 24},
    {.code = "YE", .length = 30},
  });
  // NOLINTEND(readability-magic-numbers)
  // clang-format on

  // Structural sizes for the IBAN's fixed prefix and for Slovenia's BBAN layout.
  inline constexpr std::size_t k_country_len = 2;
  inline constexpr std::size_t k_check_len   = 2;
  inline constexpr std::size_t k_prefix_len  = k_country_len + k_check_len;
  inline constexpr std::size_t k_si_bban_len = 15; // NOLINT(readability-magic-numbers)
  inline constexpr std::size_t k_si_base_len = 13; // NOLINT(readability-magic-numbers) -- bank code (5) + account number (8)
  inline constexpr int         k_mod97       = 97; // NOLINT(readability-magic-numbers)
  inline constexpr int         k_letter_base = 10; // NOLINT(readability-magic-numbers) -- A=10 .. Z=35, per ISO 13616

  // Folds one IBAN character into a running ISO 7064 MOD 97-10 remainder -- a letter expands to
  // its two-digit A=10..Z=35 value, so both digits are folded in separately.
  [[nodiscard]] constexpr std::uint64_t feed_char(std::uint64_t remainder, char c) noexcept
  {
    // NOLINTBEGIN(readability-magic-numbers) -- base-10 digit folding, not tunable values
    if (c >= 'A' && c <= 'Z')
    {
      const int v = c - 'A' + k_letter_base;
      remainder   = (remainder * 10 + static_cast<std::uint64_t>(v / 10)) % k_mod97;
      remainder   = (remainder * 10 + static_cast<std::uint64_t>(v % 10)) % k_mod97;
    }
    else remainder = (remainder * 10 + static_cast<std::uint64_t>(c - '0')) % k_mod97;
    return remainder;
    // NOLINTEND(readability-magic-numbers)
  }
} // namespace

namespace fsp::ach
{
  std::optional<std::size_t> utility::expected_length(cstr_t country_code) noexcept
  {
    const auto* const it = std::ranges::find(country_lengths, country_code, &country_len::code);
    if (it == country_lengths.end()) return std::nullopt;
    return it->length;
  }

  bool utility::iso_checksum_ok(cstr_t iban) noexcept
  {
    // ISO 13616 checksum: conceptually move the 4-character country+check-digit prefix to the
    // end, then the whole rearranged string must reduce to remainder 1 mod 97. Feeding the
    // suffix first and the prefix last achieves the same rearrangement without allocating.
    std::uint64_t remainder = 0;
    for (std::size_t i = k_prefix_len; i < iban.size(); ++i)
      remainder = feed_char(remainder, static_cast<char>(std::toupper(static_cast<unsigned char>(iban[i]))));
    for (std::size_t i = 0; i < k_prefix_len; ++i)
      remainder = feed_char(remainder, static_cast<char>(std::toupper(static_cast<unsigned char>(iban[i]))));
    return remainder == 1;
  }

  bool utility::si_national_check_ok(cstr_t bban) noexcept
  {
    // Slovenia's BBAN carries its own published national check digits: bank code + account
    // number (13 digits) followed by a 2-digit control number, computed the same way as the
    // overall IBAN checksum above -- append "00" for the control digits, reduce mod 97, and the
    // control number is 98 minus that remainder.
    if (bban.size() != k_si_bban_len) return false;
    for (char c : bban)
      if (c < '0' || c > '9') return false;

    // NOLINTBEGIN(readability-magic-numbers) -- base-10 digit folding and the mod-97 check-digit
    // formula (98 - remainder), not tunable values
    std::uint64_t remainder = 0;
    for (std::size_t i = 0; i < k_si_base_len; ++i) remainder = (remainder * 10 + static_cast<std::uint64_t>(bban[i] - '0')) % k_mod97;
    remainder = (remainder * 10) % k_mod97; // first appended placeholder digit
    remainder = (remainder * 10) % k_mod97; // second appended placeholder digit

    const int expected = 98 - static_cast<int>(remainder);
    const int actual   = ((bban[k_si_base_len] - '0') * 10) + (bban[k_si_base_len + 1] - '0');
    // NOLINTEND(readability-magic-numbers)
    return expected == actual;
  }

  bool utility::check_iban(cstr_t iban)
  {
    if (iban.size() <= k_prefix_len) return false;

    // Country + total length gate first: an unrecognized code or a mismatched length rejects
    // cheaply, before scanning the (possibly attacker-controlled) rest of the string character
    // by character. The country's own A-Z shape needs no separate check -- expected_length()
    // does an exact, case-sensitive match against the (uppercase) table, so anything that isn't
    // one of the known 2-letter codes already fails the lookup.
    const cstr_t country  = iban.substr(0, k_country_len);
    const auto   expected = expected_length(country);
    if (! expected || iban.size() != *expected) return false;

    const cstr_t check_digits = iban.substr(k_country_len, k_check_len);
    for (char c : check_digits)
      if (c < '0' || c > '9') return false;

    const cstr_t bban = iban.substr(k_prefix_len);
    for (char c : bban)
      if (std::isalnum(static_cast<unsigned char>(c)) == 0) return false;

    if (! iso_checksum_ok(iban)) return false;

    if (country == "SI" && ! si_national_check_ok(bban)) return false;

    return true;
  }

  str_t utility::print_iban(cstr_t iban)
  {
    str_t compact;
    compact.reserve(iban.size());
    for (char c : iban)
      if (std::isspace(static_cast<unsigned char>(c)) == 0)
        compact.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));

    constexpr std::size_t block = 4;
    str_t                 out;
    out.reserve(compact.size() + (compact.size() / block));
    for (std::size_t i = 0; i < compact.size(); i += block)
    {
      if (i > 0) out.push_back(' ');
      out.append(compact, i, block);
    }
    return out;
  }

  std::expected<iban_t, error_info> iban_t::parse(cstr_t s)
  {
    if (! utility::check_iban(s)) return std::unexpected(error_info::semantic("invalid_iban", fmt::format("invalid IBAN: '{}'", s)));
    return iban_t{str_t(s)};
  }
} // namespace fsp::ach
