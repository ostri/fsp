#pragma once

#include "xml_attr.hpp"
#include "xpath_set.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
// #include <ranges>
#include <span>
// #include <string>
#include <string_view>
#include <vector>

namespace fsp
{
  using date_t      = std::chrono::year_month_day;
  using ts_t        = std::chrono::sys_time<std::chrono::milliseconds>;
  using big_int_t   = std::uint64_t;
  using int_t       = std::int32_t;
  using small_int_t = std::int16_t;
  using cstr_t      = std::string_view;

  // ISO 20022 ActiveCurrencyAndAmount_SimpleType (pacs.008 XSD): totalDigits=18,
  // fractionDigits=5.
  inline constexpr std::size_t amount_scale = 5; // NOLINT(readability-magic-numbers)

  // 10^amount_scale, computed from amount_scale itself so the two can never drift apart.
  inline constexpr big_int_t amount_scale_factor = []
  {
    big_int_t f = 1;
    for (std::size_t i = 0; i < amount_scale; ++i) f *= 10; // NOLINT(readability-magic-numbers)
    return f;
  }();

  /**
   * @brief Fixed-point ISO 20022 decimal amount: value scaled by 10^amount_scale, held as a
   * plain integer.
   * @details A distinct type from big_int_t on purpose -- convert_scalar() (see
   * reflection.hpp) dispatches on FieldType, and a bare `using amount_t = big_int_t` would BE
   * big_int_t (aliases don't create new types in C++), so the two could never be told apart
   * there; a field declared amount_t would then silently take big_int_t's plain
   * std::from_chars path and lose its fraction. The wrapper also documents at the call site
   * that this integer isn't a plain count -- it's scaled and needs dividing by
   * 10^amount_scale (or a decimal-aware formatter) to read back as money.
   */
  struct amount_t
  {
    big_int_t value = 0; // NOLINT(misc-non-private-member-variables-in-classes)
  };

  /**
   * @brief Parses an ISO 20022 decimal amount (xs:decimal, ActiveCurrencyAndAmount_SimpleType)
   * into amount_t -- e.g. "1.00" -> {100000}, "123.45" -> {12345000}, a bare "7" -> {700000}.
   * @details Splits on '.', right-pads a missing/short fractional part with '0' up to
   * amount_scale digits and truncates a longer one (the schema itself never emits more than
   * amount_scale digits there, so this only guards malformed input), then parses the
   * digit-only concatenation of both parts as one plain integer -- the schema's own
   * totalDigits=18 ceiling guarantees the result always fits big_int_t (a 64-bit integer
   * holds up to ~1.8e19, well past 1e18).
   * @param s well-formed ISO 20022 decimal amount text (not validated further)
   */
  inline amount_t parse_iso_amount(cstr_t s)
  {
    auto   dot       = s.find('.');
    cstr_t int_part  = (dot == cstr_t::npos) ? s : s.substr(0, dot);
    cstr_t frac_part = (dot == cstr_t::npos) ? cstr_t{} : s.substr(dot + 1);
    if (frac_part.size() > amount_scale) frac_part = frac_part.substr(0, amount_scale);

    // Fixed-width digit concatenation -- the pointer arithmetic and array indexing below are
    // inherent to building the scaled integer's digit string without allocating; 24 is a
    // generous cap on 18 significant digits (see totalDigits=18 above), not a tunable value.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-bounds-constant-array-index,
    // readability-magic-numbers)
    std::array<char, 24> buf{};
    std::size_t          pos = 0;
    for (char c : int_part) buf[pos++] = c;
    for (char c : frac_part) buf[pos++] = c;
    for (std::size_t i = frac_part.size(); i < amount_scale; ++i) buf[pos++] = '0';

    big_int_t v = 0;
    std::from_chars(buf.data(), buf.data() + pos, v);
    return amount_t{v};
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, cppcoreguidelines-pro-bounds-constant-array-index,
    // readability-magic-numbers)
  }

  /**
   * @brief Parses an ISO 20022 ISODate value (xs:date) into date_t.
   * @details Expects the fixed-width YYYY-MM-DD form. Not a general ISO 8601 parser: uses
   * std::from_chars on fixed offsets rather than a stream, to match convert_scalar()'s other
   * conversions (see reflection.hpp) and avoid locale/stream overhead on a format that never
   * varies in width.
   * @param s well-formed ISO 8601 date text (not validated further)
   */
  inline date_t parse_iso_date(cstr_t s)
  {
    // Fixed-width ISO 8601 layout: the literal byte offsets and the pointer arithmetic to
    // reach them are inherent to a from_chars-based fixed-format parse.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers, bugprone-suspicious-stringview-data-usage)
    int y  = 0;
    int mo = 0;
    int d  = 0;
    std::from_chars(s.data(), s.data() + 4, y);
    std::from_chars(s.data() + 5, s.data() + 7, mo);
    std::from_chars(s.data() + 8, s.data() + 10, d);
    return std::chrono::year{y} / mo / d;
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers, bugprone-suspicious-stringview-data-usage)
  }

  /**
   * @brief Parses an ISO 20022 ISODateTime value (xs:dateTime) into ts_t.
   * @details Expects the fixed-width form YYYY-MM-DDThh:mm:ss (the date part read via
   * parse_iso_date()), followed by an optional .sss millisecond fraction and an optional zone
   * marker -- 'Z', a +/-hh:mm offset, or nothing at all. A bare (zone-less) value is treated
   * as UTC, since these messages carry no separate context to interpret it against.
   * @param s well-formed ISO 8601 date-time text (not validated further)
   */
  inline ts_t parse_iso_datetime(cstr_t s)
  {
    auto date = parse_iso_date(s.substr(0, 10)); // NOLINT(readability-magic-numbers)

    // Fixed-width ISO 8601 layout (see doc above): the literal byte offsets and the pointer
    // arithmetic to reach them are inherent to a from_chars-based fixed-format parse.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers, bugprone-suspicious-stringview-data-usage)
    int h  = 0;
    int mi = 0;
    int se = 0;
    std::from_chars(s.data() + 11, s.data() + 13, h);
    std::from_chars(s.data() + 14, s.data() + 16, mi);
    std::from_chars(s.data() + 17, s.data() + 19, se);

    auto tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::sys_days{date} + std::chrono::hours{h} +
                                                                      std::chrono::minutes{mi} + std::chrono::seconds{se});

    std::size_t pos = 19;
    if (pos < s.size() && s[pos] == '.')
    {
      int ms = 0;
      std::from_chars(s.data() + pos + 1, s.data() + pos + 4, ms);
      tp += std::chrono::milliseconds{ms};
      pos += 4;
    }
    if (pos < s.size() && s[pos] != 'Z')
    {
      char sign = s[pos];
      int  oh   = 0;
      int  om   = 0;
      std::from_chars(s.data() + pos + 1, s.data() + pos + 3, oh);
      std::from_chars(s.data() + pos + 4, s.data() + pos + 6, om);
      auto offset = std::chrono::hours{oh} + std::chrono::minutes{om};
      tp -= (sign == '+') ? offset : -offset;
    }
    return tp;
    // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic, readability-magic-numbers, bugprone-suspicious-stringview-data-usage)
  }

  // Fixed capacity for the m_* (array-cardinality) field types below.
  inline constexpr std::size_t max_values = 10; // NOLINT(readability-magic-numbers)

  /**
   * @brief o_ and m_ prefixed aliases: one pair per scalar type above, meant to eventually let
   * a field's own C++ type carry its cardinality instead of a leading marker character in the
   * xpath annotation text (see field_attr_of() in reflection.hpp for today's marker-based
   * scheme). An o_-prefixed alias is std::optional<T> (0 or 1 values); an m_-prefixed alias is
   * a fixed-capacity std::array<T, max_values> (repeated values, capped at max_values). Not
   * yet wired into convert_scalar()/materialize<T>() -- for now these are just the named
   * types; making field_attr_of()/materialize<T>() actually read cardinality off FieldType
   * instead of the annotation string is separate follow-up work.
   */
  using o_str_t       = std::optional<str_t>;
  using o_big_int_t   = std::optional<big_int_t>;
  using o_int_t       = std::optional<int_t>;
  using o_small_int_t = std::optional<small_int_t>;
  using o_date_t      = std::optional<date_t>;
  using o_ts_t        = std::optional<ts_t>;
  using o_amount_t    = std::optional<amount_t>;

  using m_str_t       = std::array<str_t, max_values>;
  using m_big_int_t   = std::array<big_int_t, max_values>;
  using m_int_t       = std::array<int_t, max_values>;
  using m_small_int_t = std::array<small_int_t, max_values>;
  using m_date_t      = std::array<date_t, max_values>;
  using m_ts_t        = std::array<ts_t, max_values>;
  using m_amount_t    = std::array<amount_t, max_values>;

  // --- main structure -----------------------------------------------------------------


  // --- proc_data ----------------------------------------------------------------------
  // Note: xpaths stays std::vector -- proc_data is not constexpr, it's a runtime structure.
  // A constexpr proc_data would need std::array<xpath_node_struct, N> instead.
  struct proc_data
  {
    fsp::xpath_set              targets; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<fsp::xpath_set> xpaths;  // NOLINT(misc-non-private-member-variables-in-classes)
    // Indexed by seg_type() (same declaration-order indexing as xpaths above): is_header[i] is
    // true iff that schema class derives from fsp::hdr_seg_schema (see reflection.hpp's own doc
    // comment) instead of plain fsp::seg_schema. Filled by proc_data_of() from each schema
    // class's own static consteval is_header() -- a caller never sets this by hand. Consulted by
    // doc_cutter::init() to build the dense, index-by-seg_type() lookup Handler's endElement()
    // uses to route header segments into the priority queue (see docs/importer_usage.md's own
    // "Header segments are processed first" section).
    std::vector<bool> is_header; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] str_t dump(int offs = 0) const
    { return fmt::format("{0}targets:{1}\n{0}xpaths.size:{2}", str_t(offs, ' '), targets.dump(offs), xpaths.size()); }
  };

  // --- build --------------------------------------------------------------------------
  [[nodiscard]] constexpr xpath_set build(std::span<const raw_attr> raw_paths, std::span<const ns> ns_arr) { return {raw_paths, ns_arr}; }


} // namespace fsp

/**
 * @brief Formats an fsp::amount_t back as a plain decimal string, e.g. {112345} -> "1.12345".
 * @details Always prints exactly amount_scale fraction digits (zero-padded) -- the inverse of
 * fsp::parse_iso_amount(), so round-tripping an ISO 20022 amount through amount_t and back to
 * text never re-introduces the plain-integer truncation amount_t exists to avoid.
 */
template <>
struct fmt::formatter<fsp::amount_t>
{
  static constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }

  static auto format(const fsp::amount_t& a, format_context& ctx)
  {
    return fmt::format_to(
      ctx.out(), "{}.{:0{}}", a.value / fsp::amount_scale_factor, a.value % fsp::amount_scale_factor, fsp::amount_scale);
  }
};
