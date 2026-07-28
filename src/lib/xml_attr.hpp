#pragma once

#include "compile_error.hpp"
#include "xpath_el.hpp"
#include <cstddef>
#include <fmt/format.h>
#include <ranges>
#include <span>
#include <string>
#include <array>
#include <algorithm>

namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;

  struct ns
  {
    cstr_t prefix;
    cstr_t uri;
  };

  // --- raw attribute definition (compile-time) ----------------------------------------
  /**
   * @brief path may carry markers understood natively by xml_attr's parsing,
   * in addition to the raw is_opt flag below (kept for hand-written tables
   * that don't use markers at all):
   *
   *   "path"     required, exactly one value, tag
   *   "?path"    optional, zero or one value             (leading, whole-path)
   *   "*path"    optional, zero or more values           (leading, whole-path)
   *   "+path"    required, one or more values            (leading, whole-path)
   *   ".../@tag" or ".../@ns:tag"   tag is an attribute, not an element -- '@'
   *              is looked for on whichever segment carries it (conventionally
   *              the last one, mimicking xpath's ".../@Ccy"), not just position 0
   *              of the whole string.
   *
   * The two are independent and combine freely, e.g. "*CdtTrfTxInf/.../@Ccy".
   */
  struct raw_attr
  {
    constexpr bool operator==(const raw_attr& o) const { return (o.name == name) && (o.path == path); }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t name;
    cstr_t path;
    bool   is_opt = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  constexpr const std::size_t max_xpath_len = 10; // maximum length of the xpath (in segmetns) /a/b/c/...
  using raw_inputs                          = std::span<const raw_attr>;
  using xpath_vec                           = std::array<xpath_el, max_xpath_len>;
  using xpath_span                          = std::span<const xpath_el>;

  // parse_xpath_to_elements result — no std::vector
  struct xpath_parse_result
  {
    xpath_vec   elements{};
    std::size_t size = 0;
  };

  // --- xml attribute (runtime friendly, built at compile time) ------------------------
  struct xml_attr
  {
  public:
    constexpr xml_attr() = default;
    constexpr xml_attr(std::size_t original_ndx, const raw_attr& raw, std::span<const ns> ns_arr);

    // full_xpath and full_xpath_with_uri are not constexpr, since fmt::format is not constexpr
    [[nodiscard]] str_t full_xpath() const;
    [[nodiscard]] str_t full_xpath_with_uri() const;

    [[nodiscard]] constexpr cstr_t      name() const { return name_; }
    [[nodiscard]] constexpr cstr_t      path() const { return path_; }
    [[nodiscard]] constexpr bool        is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool        is_attr() const { return ! attr_.tag.empty(); }
    [[nodiscard]] constexpr bool        is_array() const { return is_array_; }
    [[nodiscard]] constexpr xpath_span  xpath() const { return {xpath_.data(), xpath_size_}; }
    [[nodiscard]] constexpr std::size_t size() const { return xpath_size_; }
    [[nodiscard]] constexpr bool        is_last(std::size_t ndx) const { return ndx == xpath_size_ - 1; }
    [[nodiscard]] constexpr cstr_t      attr_name() const { return attr_.tag; }
    [[nodiscard]] constexpr cstr_t      attr_uri() const { return attr_.ns; }
    [[nodiscard]] constexpr xpath_el    last() const { return xpath_.back(); }
    [[nodiscard]] constexpr std::size_t original_ndx() const { return original_ndx_; }

    // dump() ni constexpr — uporablja fmt::format
    [[nodiscard]] str_t dump(int offs = 0) const;
  private:
    static constexpr cstr_t             trim_xpath(cstr_t str);
    static constexpr cstr_t             uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr);
    static constexpr xpath_parse_result parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr);
  private:
    cstr_t      name_;
    cstr_t      path_;
    bool        is_array_ = false; // is this xpath value multivalue (array '*')
    bool        is_opt_   = false; // is this xpath value optional ?
    xpath_vec   xpath_{};          // array of xpath segemtns minus optional attribute xpath
    xpath_el    attr_{};           // attribute definition; for regular elements attr_.tag is empty
    std::size_t xpath_size_   = 0; // actuaal length of the xpath minus attribute (we need it because of std::array)
    std::size_t original_ndx_ = 0; // original index of xpath as provided by programmer (obsolete)
  };
  /**
   * @brief parse xpath string to array of xpath_vec and expand prefixes to uri
   *
   * @param input input string
   * @param ns_arr array of translations prefix -> uri
   * @return constexpr xpath_parse_result std::array with actual length
   */
  constexpr xpath_parse_result xml_attr::parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr)
  {
    if (input.empty()) throw compile_error("empty xpath");
    xpath_parse_result result{};
    for (auto segment_range : input | std::ranges::views::split('/'))
    { // split string on / into segments and process the segments
      cstr_t segment{segment_range};
      if (segment.empty()) continue;
      if (result.size >= max_xpath_len) throw compile_error("xpath exceeds max_xpath_len");
      xpath_el element{};
      // '@' marks this segment as an attribute (mimicking xpath's ".../@Ccy");
      // strip it up front so the rest parses exactly like a normal element --
      // this also makes "@ns:tag" (attribute with an explicit prefix) work,
      // which the old "check only when there's no colon" version could not.
      bool is_attr_segment = segment.starts_with('@');
      if (is_attr_segment) segment = segment.substr(1);

      auto colon_pos = segment.find(':');
      if (colon_pos != cstr_t::npos)
      { // we found the colon in segment
        element.ns  = uri_from_prefix(segment.substr(0, colon_pos), ns_arr);
        element.tag = segment.substr(colon_pos + 1);
      }
      else
      { // there is no colon in segment: elements fall back to the default
        // namespace, unprefixed attributes never do (XML namespace rules)
        element.ns  = is_attr_segment ? cstr_t{} : uri_from_prefix("", ns_arr);
        element.tag = segment;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      result.elements[result.size++] = element;
    }
    return result;
  }

  [[nodiscard]] inline str_t xml_attr::dump(int offs) const
  {
    str_t msg_xpath;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::size_t i = 0; i < xpath_size_; ++i) msg_xpath += fmt::format("[{}:{}]/", xpath_[i].tag, xpath_[i].ns);

    return fmt::format("{}name: {:15} path: {:40} is_array: {:5} is_opt {:5} attr: {}:{:15} xpath size:{:2} original ndx:{:2} {}",
                       str_t(offs, ' '),
                       name_,
                       path_,
                       is_array_,
                       is_opt_,
                       attr_.ns,
                       attr_.tag,
                       xpath_size_,
                       original_ndx_,
                       msg_xpath);
  }
  /**
   * @brief trim xpath string of leading/trailing ws and slashes
   *
   * @param str input xpath string (i.e. /a/b/c)
   * @return constexpr cstr_t trimmed xpath string
   */
  constexpr cstr_t xml_attr::trim_xpath(cstr_t str)
  {
    auto        is_ws_or_slash = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/'; };
    const auto* start          = std::ranges::find_if_not(str, is_ws_or_slash);
    if (start == str.end()) throw compile_error("Empty xpath after trimming");
    const auto* end = std::ranges::find_if_not(str | std::views::reverse, is_ws_or_slash).base();
    return {start, end};
  }

  constexpr cstr_t xml_attr::uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
      if (el.prefix == prefix) return el.uri;
    throw compile_error("Prefix has no matching definition in ns structure");
  }

  constexpr xml_attr::xml_attr(std::size_t original_ndx, const raw_attr& raw, std::span<const ns> ns_arr)
  : name_(raw.name)
  , original_ndx_(original_ndx)
  {
    // leading whole-path cardinality marker: ?, * or + (see raw_attr comment above)
    cstr_t working      = raw.path;
    bool   marker_opt   = false;
    bool   marker_array = false;
    if (! working.empty())
    {
      char c = working.front();
      if (c == '?' || c == '*' || c == '+')
      {
        marker_opt   = (c == '?' || c == '*');
        marker_array = (c == '*' || c == '+');
        working      = working.substr(1);
      }
    }
    path_   = trim_xpath(working);
    is_opt_ = raw.is_opt || marker_opt;

    // '@' anywhere in the (marker-stripped) path means it resolves to an
    // attribute -- parse_xpath_to_elements() already found and consumed it
    // per-segment, this just tells us whether the *last* element is that one.
    bool marker_attr = path_.contains('@');

    auto parsed = parse_xpath_to_elements(path_, ns_arr);
    xpath_size_ = parsed.size;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::copy(parsed.elements.begin(), parsed.elements.begin() + xpath_size_, xpath_.begin());
    auto  last_pos = xpath_size_ - 1;  // index of last element in the xpath
    auto& last_el  = xpath_[last_pos]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    if (marker_attr)
    {
      attr_ = last_el; // ns/tag already resolved correctly (no '@') by parse_xpath_to_elements
      xpath_size_--;
    }
    else if (marker_array || last_el.tag.starts_with('*'))
    { // last_el.tag.starts_with('*') is the old per-segment convention, kept
      // for hand-written raw_attr tables that embed '*' instead of using the
      // leading marker (e.g. ".../*BICFI")
      is_array_ = true;
      if (last_el.tag.starts_with('*'))
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
        xpath_[last_pos] = xpath_el{.ns = last_el.ns, .tag = last_el.tag.substr(1)}; // strip '*'
    }
  }

  [[nodiscard]] inline str_t xml_attr::full_xpath() const
  {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
    str_t tmp;
    if (is_array())
    {
      auto last_el_ndx = xpath_size_ - 1;
      for (std::size_t i = 0; i < last_el_ndx; ++i) tmp += fmt::format("/{}", xpath_[i].tag);
      tmp += fmt::format("/*{}", xpath_[last_el_ndx].tag);
    }
    else
    {
      for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}", xpath_[i].tag);
      if (is_attr()) tmp += fmt::format("/@{}", attr_name());
    }
    return tmp;
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  }

  [[nodiscard]] inline str_t xml_attr::full_xpath_with_uri() const
  {
    str_t tmp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}:{}", xpath_[i].ns, xpath_[i].tag);
    if (is_attr()) tmp += fmt::format("/@{}:{}", attr_uri(), attr_name());
    else if (is_array()) tmp += "/*";
    return tmp;
  }
} // namespace fsp
