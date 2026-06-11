#pragma once

#include "compile_error.hpp"
#include "xpath_el.hpp"
#include <cstddef>
#include <fmt/format.h>
#include <ranges>
#include <span>
#include <string>
#include <array>

namespace fsp
{
  using cstr_t = std::string_view;

  struct ns
  {
    cstr_t prefix;
    cstr_t uri;
  };

  // --- raw attribute definition (compile-time) ----------------------------------------
  struct raw_attr
  {
    constexpr bool operator==(const raw_attr& o) const { return (o.name == name) && (o.path == path); }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t name;
    cstr_t path;
    bool   is_opt = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  using raw_inputs                          = std::span<const raw_attr>;
  constexpr const std::size_t max_xpath_len = 10;
  using xpath_vec                           = std::array<xpath_el, max_xpath_len>;
  using xpath_span                          = std::span<const xpath_el>;

  // Pomožna struktura za rezultat parse_xpath_to_elements — brez std::vector
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

    // full_xpath in full_xpath_with_uri nista constexpr — fmt::format ni constexpr
    [[nodiscard]] std::string full_xpath() const;
    [[nodiscard]] std::string full_xpath_with_uri() const;

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
    [[nodiscard]] std::string dump(int offs = 0) const;
  private:
    static constexpr cstr_t trim_xpath(cstr_t str);
    static constexpr cstr_t uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr);
    // FIX 1: vrne xpath_parse_result namesto std::vector — brez heap alokacije
    static constexpr xpath_parse_result parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr);
  private:
    cstr_t      name_;
    cstr_t      path_;
    bool        is_array_ = false;
    bool        is_opt_   = false;
    xpath_vec   xpath_{};
    xpath_el    attr_{};
    std::size_t xpath_size_   = 0;
    std::size_t original_ndx_ = 0;
    // FIX 5: std::string normalized_path_ ODSTRANJEN — preprečeval je constexpr shranjevanje.
    //        Uporabi full_xpath_with_uri() on-demand kadar je potrebno.
  };


  // FIX 1: parse_xpath_to_elements vrne fiksni array + size, brez std::vector
  constexpr xpath_parse_result xml_attr::parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr)
  {
    if (input.empty()) throw compile_error("empty xpath");

    xpath_parse_result result{};

    for (auto segment_range : input | std::ranges::views::split('/'))
    {
      cstr_t segment{segment_range};
      if (segment.empty()) continue;

      if (result.size >= max_xpath_len) throw compile_error("xpath exceeds max_xpath_len");

      xpath_el element{};
      auto     colon_pos = segment.find(':');

      if (colon_pos != cstr_t::npos)
      {
        element.ns  = uri_from_prefix(segment.substr(0, colon_pos), ns_arr);
        element.tag = segment.substr(colon_pos + 1);
      }
      else
      {
        element.ns  = segment.starts_with('@') ? "" : uri_from_prefix("", ns_arr);
        element.tag = segment;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      result.elements[result.size++] = element;
    }
    return result;
  }

  // dump() ni constexpr — fmt::format alokira
  [[nodiscard]] inline std::string xml_attr::dump(int offs) const
  {
    std::string msg_xpath;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::size_t i = 0; i < xpath_size_; ++i) msg_xpath += fmt::format("[{}:{}]/", xpath_[i].tag, xpath_[i].ns);

    return fmt::format("{}name: {:15} path: {:40} is_array: {:5} is_opt {:5} attr: {}:{:15} xpath size:{:2} original ndx:{:2} {}",
                       std::string(offs, ' '),
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

  constexpr cstr_t xml_attr::trim_xpath(cstr_t str)
  {
    auto is_ws_or_slash = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/'; };

    const auto* start = std::ranges::find_if_not(str, is_ws_or_slash);
    if (start == str.end())
      // FIX 4: ne kličemo fmt::format v constexpr throw — string literal zadostuje
      throw compile_error("Empty xpath after trimming");

    const auto* end = std::ranges::find_if_not(str | std::views::reverse, is_ws_or_slash).base();
    return {start, end};
  }

  // FIX 3: runtime_error zamenjano s compile_error — constexpr-kompatibilno
  constexpr cstr_t xml_attr::uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
      if (el.prefix == prefix) return el.uri;

    // V constexpr kontekstu: vsakršna throw pot mora biti compile_error.
    // Podrobno diagnostično sporočilo z fmt::format ni možno brez runtime poti —
    // napaka bo zaznavna prek compile_error sporočila in stacka pri prevajanju.
    throw compile_error("Prefix has no matching definition in ns structure");
  }

  // FIX 2: normalized_path_ ni več data member — konstruktor je sedaj čist constexpr
  constexpr xml_attr::xml_attr(std::size_t original_ndx, const raw_attr& raw, std::span<const ns> ns_arr)
  : name_(raw.name)
  , path_(trim_xpath(raw.path))
  , is_opt_(raw.is_opt)
  , original_ndx_(original_ndx)
  {
    // FIX 1+2: parse rezultat gre neposredno v array — brez vector, brez UB
    auto parsed = parse_xpath_to_elements(path_, ns_arr);
    xpath_size_ = parsed.size;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    std::copy(parsed.elements.begin(), parsed.elements.begin() + xpath_size_, xpath_.begin());

    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    auto& el = xpath_[xpath_size_ - 1];
    if (el.tag.starts_with('@'))
    {
      attr_ = xpath_el{.ns = el.ns, .tag = el.tag.substr(1)};
      xpath_size_--;
    }
    else if (el.tag.starts_with('*'))
    {
      is_array_ = true;
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      xpath_[xpath_size_ - 1] = xpath_el{.ns = el.ns, .tag = el.tag.substr(1)};
    }
  }

  // full_xpath ni constexpr — fmt::format ni constexpr (alokira string na heap)
  [[nodiscard]] inline std::string xml_attr::full_xpath() const
  {
    std::string tmp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}", xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}", attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }

  [[nodiscard]] inline std::string xml_attr::full_xpath_with_uri() const
  {
    std::string tmp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}:{}", xpath_[i].ns, xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}:{}", attr_uri(), attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }

} // namespace fsp
