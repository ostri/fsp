#pragma once

#include <array>
#include <chrono>
#include <fmt/format.h>
#include <string_view>
#include <vector>
#include <span>
#include <algorithm>
#include <exception>
#include <ranges>


namespace fsp
{
  using cstr_t = std::string_view;
  class compile_error : public std::exception
  {
  public:
    constexpr explicit compile_error(const char* msg)
    : message(msg)
    {
    }
    [[nodiscard]] constexpr const char* what() const noexcept override { return message; }
  private:
    const char* message;
  };

  struct ns
  {
    cstr_t prefix;
    cstr_t uri;
  };

  struct xpath_el
  {
    cstr_t ns;
    cstr_t tag;
  };

  struct raw_attr
  {
  public:
    [[nodiscard]] constexpr size_t        xpath_size() const { return xpath_size(path); }
    [[nodiscard]] static constexpr size_t xpath_size(cstr_t path)
    {
      char ch = '/';
      if (path.empty()) return 0;
      if (path[0] == ch) return std::ranges::count(path.substr(1), ch) + 1;
      return std::ranges::count(path, ch) + 1;
    }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t name;
    cstr_t path;
    bool   is_opt = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };
  /////////////////////////////////////////////////////////////////////////////
  template <size_t N>
  struct raw_inputs_container : std::array<fsp::raw_attr, N>
  {
    // Konstruktor, ki sprejme standardni std::array (1 argument)
    constexpr raw_inputs_container(std::array<fsp::raw_attr, N> arr) // NOLINT(google-explicit-constructor)
    : std::array<fsp::raw_attr, N>(arr)
    {
    }

    [[nodiscard]] constexpr size_t get_max_xpath_len() const noexcept
    {
      size_t max_d = 0;
      for (const auto& attr : *this) { max_d = std::max(max_d, attr.xpath_size()); }
      return max_d;
    }
  };

  // Dedukcijski vodnik, ki pravilno izpelje velikost 'N' iz std::array
  template <size_t N>
  raw_inputs_container(std::array<fsp::raw_attr, N>) -> raw_inputs_container<N>;
  //////////////////////////////////////////////////////////////////////////////////

  // Pomožna funkcija za odstranjevanje presledkov (White Spaces) in poševnic
  constexpr cstr_t trim_xpath(cstr_t str)
  {
    auto is_ws_or_slash = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/'; };

    // Poišči začetek brez WS in /
    const auto* start = std::ranges::find_if_not(str, is_ws_or_slash);
    if (start == str.end()) return {};

    // Poišči konec brez WS in /
    const auto* end = std::ranges::find_if_not(str | std::views::reverse, is_ws_or_slash).base();

    return {start, end};
  }
  consteval cstr_t uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
    {
      if (el.prefix == prefix) return el.uri;
    }
    throw compile_error("Prefix has no matching definition in ns structure.");
  }
  consteval std::vector<xpath_el> parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr)
  {
    // 1. Odstrani vodilne in končne presledke ter poševnice
    cstr_t trimmed = trim_xpath(input);
    if (trimmed.empty()) return {};

    std::vector<xpath_el> result;

    // 2. C++20/C++23 views za razbijanje niza z delimiterjem '/'
    // views::split ustvari pod-razpone (subranges) posameznih segmentov
    for (auto segment_range : trimmed | std::views::split('/'))
    {
      // Pretvorba trenutnega segmenta nazaj v string_view (C++23 konstrukcija)
      cstr_t segment{segment_range};

      if (segment.empty()) continue;

      xpath_el element{};

      // 3. Razbijanje posameznega segmenta glede na dvopičje ':'
      size_t colon = segment.find(':');
      if (colon != cstr_t::npos)
      {
        element.ns  = uri_from_prefix(segment.substr(0, colon), ns_arr);
        element.tag = segment.substr(colon + 1);
      }
      else
      {
        element.ns  = uri_from_prefix("", ns_arr);
        element.tag = segment;
      }

      result.push_back(element);
    }

    return result;
  }
  template <size_t xpath_len>
  struct xml_attr
  {
  public:
    constexpr xml_attr() = default;
    constexpr xml_attr(raw_attr raw, const auto& ns_arr)
    : name_(raw.name)
    , path_(raw.path)
    , is_opt_(raw.is_opt)
    {
      auto tmp = parse_xpath_to_elements(raw.path, ns_arr); // NOLINT(cppcoreguidelines-prefer-member-initializer)
      for (std::size_t cnt = 0; cnt < tmp.size(); cnt++) xpath_[cnt] = tmp[cnt];
      if (xpath_size() > 0)
      {
        auto& last_el = xpath_.at(xpath_size() - 1);
        if (last_el.tag.starts_with('@'))
        {
          is_attr_    = true;
          last_el.tag = last_el.tag.substr(1);
        }
        else if (last_el.tag.starts_with('*'))
        {
          is_array_   = true;
          last_el.tag = last_el.tag.substr(1);
        }
      }
    }
    [[nodiscard]] constexpr cstr_t    name() const { return name_; }
    [[nodiscard]] constexpr cstr_t    path() const { return path_; }
    [[nodiscard]] constexpr bool      is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool      is_attr() const { return is_attr_; }
    [[nodiscard]] constexpr bool      is_array() const { return is_array_; }
    [[nodiscard]] constexpr auto      xpath() const { return xpath_; }
    [[nodiscard]] constexpr auto&     xpath() { return xpath_; }
    [[nodiscard]] constexpr size_t    xpath_size() const { return raw_attr::xpath_size(path_); }
    [[nodiscard]] constexpr xpath_el& last() { return xpath_.at(xpath_size() - 1); }
  private:
    cstr_t                          name_;
    cstr_t                          path_;
    bool                            is_attr_  = false;
    bool                            is_array_ = false;
    bool                            is_opt_   = false;
    std::array<xpath_el, xpath_len> xpath_{};
  };

  using date_t = std::chrono::year_month_day;

  consteval size_t get_xpaths_max_depth(std::span<const raw_attr> attrs)
  {
    size_t max_d = 0;
    for (const auto& attr : attrs) { max_d = std::max(max_d, attr.xpath_size()); }
    return max_d;
  }

  struct path_node
  {
    xpath_el node;
    int      child = -1;
    int      next  = -1;
  };

  template </*size_t path_node_size,*/ size_t paths_size, size_t xpath_size>
  class path_node_struct
  {
  public:
    consteval path_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
    {
      for (size_t i = 0; i < inputs.size(); ++i) { data[i] = xml_attr<xpath_size>(inputs[i], ns_arr); }
    }
    constexpr xml_attr<xpath_size> operator[](size_t ndx) const { return data[ndx]; }
    constexpr xml_attr<xpath_size> operator[](const cstr_t ndx) const
    {
      for (const auto& el : data)
        if (el.name() == ndx) return el;
      throw compile_error(fmt::format("unknown path '{}'.", ndx).data());
    }
    constexpr auto                 begin() const { return data.begin(); }
    constexpr auto                 end() const { return data.end(); }
    [[nodiscard]] constexpr size_t size() const { return paths_size; }
  private:
    std::array<xml_attr<xpath_size>, paths_size> data;
    //    std::array<path_node, path_node_size>        nodes;
  };


} // namespace fsp
