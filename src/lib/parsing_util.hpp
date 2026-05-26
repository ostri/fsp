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
    [[nodiscard]] constexpr std::size_t        xpath_size() const { return xpath_size(path); }
    [[nodiscard]] static constexpr std::size_t xpath_size(cstr_t path)
    {
      char ch = '/';
      if (path.empty()) return 0;
      if (path[0] == ch) return std::ranges::count(path.substr(1), ch) + 1;
      return std::ranges::count(path, ch) + 1;
    }
    bool operator==(const raw_attr& o) const { return (o.name == name) && (o.path == path); }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t name;           // value name
    cstr_t path;           // xml path to the value (ns::tag1/ns2::tag2/.../nsn:tagn)
    bool   is_opt = false; // is the value optional?
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };
  /////////////////////////////////////////////////////////////////////////////
  template <std::size_t N>
  struct raw_inputs_container : std::array<raw_attr, N>
  {
    // Konstruktor, ki sprejme standardni std::array (1 argument)
    constexpr raw_inputs_container(std::array<raw_attr, N> arr) // NOLINT(google-explicit-constructor)
    : std::array<raw_attr, N>(sort_helper(arr))
    {
    }
    [[nodiscard]] static constexpr std::array<raw_attr, N> sort_helper(std::array<raw_attr, N> arr) noexcept
    {
      std::ranges::sort(arr, [](const raw_attr& a, const raw_attr& b) { return a.path < b.path; });
      return arr;
    }
    [[nodiscard]] constexpr std::size_t get_max_xpath_len() const noexcept
    {
      size_t max_d = 0;
      for (const auto& attr : *this) { max_d = std::max(max_d, attr.xpath_size()); }
      return max_d;
    }
    // number of all tags over the structure
    [[nodiscard]] constexpr std::size_t get_total_xpath_len() const noexcept
    {
      return std::ranges::fold_left(*this, std::size_t{0}, [](std::size_t sum, const auto& attr) { return sum + attr.xpath_size(); });
    }
  };

  // Dedukcijski vodnik, ki pravilno izpelje velikost 'N' iz std::array
  template <std::size_t N>
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
      auto colon = segment.find(':');
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
  template <std::size_t xpath_len>
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
        if (last_el.tag.starts_with('@')) is_attr_ = true;              // is attribute ?
        else if (last_el.tag.starts_with('*')) is_array_ = true;        // has array of values?
        if (is_attr_ || is_array_) last_el.tag = last_el.tag.substr(1); // strip the marker for attributes and arrays
      }
    }
    [[nodiscard]] constexpr cstr_t      name() const { return name_; }
    [[nodiscard]] constexpr cstr_t      path() const { return path_; }
    [[nodiscard]] constexpr bool        is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool        is_attr() const { return is_attr_; }
    [[nodiscard]] constexpr bool        is_array() const { return is_array_; }
    [[nodiscard]] constexpr auto        xpath() const { return xpath_; }
    [[nodiscard]] constexpr auto&       xpath() { return xpath_; }
    [[nodiscard]] constexpr std::size_t xpath_size() const { return raw_attr::xpath_size(path_); }
    [[nodiscard]] constexpr xpath_el&   last() { return xpath_.at(xpath_size() - 1); }
  private:
    cstr_t                          name_;
    cstr_t                          path_;
    bool                            is_attr_  = false;
    bool                            is_array_ = false;
    bool                            is_opt_   = false;
    std::array<xpath_el, xpath_len> xpath_{};
  };

  using date_t = std::chrono::year_month_day;

  consteval std::size_t get_xpaths_max_depth(std::span<const raw_attr> attrs)
  {
    std::size_t max_d = 0;
    for (const auto& attr : attrs) { max_d = std::max(max_d, attr.xpath_size()); }
    return max_d;
  }

  class path_node
  {
  public:
    static constexpr const auto max_int = std::numeric_limits<std::size_t>::max();
    constexpr path_node()
    : fsp::path_node("", "")
    {
    }
    constexpr path_node(cstr_t uri, cstr_t tag, std::size_t xp_node, std::size_t next_xpath)
    : uri_(uri)
    , tag_(tag)
    , xp_node_(xp_node)
    , next_xpath_(next_xpath)
    {
    }
    constexpr path_node(cstr_t uri, cstr_t tag)
    : path_node(uri, tag, max_int, max_int)
    {
    }
    [[nodiscard]] constexpr std::size_t xp_node() const { return xp_node_; }
    [[nodiscard]] constexpr std::size_t next_xpath() const { return next_xpath_; }
    [[nodiscard]] constexpr bool        is_xpath_last_node() const { return xp_node_ == max_int; }
  private:
    cstr_t      uri_;                  // tag's uri
    cstr_t      tag_;                  // tag name
    std::size_t xp_node_    = max_int; // xpath node or max_int if this is the end of the xpath
    std::size_t next_xpath_ = max_int; // next xpath
  };
  // template </*std::size_t path_node_size,*/ std::size_t paths_size, std::size_t xpath_size>
  template <const auto& ra>
  class path_node_struct
  {
  public:
    static constexpr const auto xpath_size          = ra.get_max_xpath_len();
    static constexpr const auto paths_size          = ra.size();
    static constexpr const auto path_node_deck_size = ra.get_total_xpath_len();
    consteval path_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
    {
      for (std::size_t i = 0; i < inputs.size(); ++i)
      {
        auto tmp = xml_attr<xpath_size>(inputs[i], ns_arr);
        data[i]  = tmp;
        // push_xpath(tmp.xpath().begin(), deck, 0);
      }
    }
    consteval std::size_t get_new_node() { return first_free_deck_++; }
    // constexpr std::size_t push_xpath(const auto& xpath_el, auto& deck, std::size_t deck_pos)
    // {
    //   if ((xpath_el.uri() == deck[deck_pos].uri()) && (xpath_el.tag() == deck[deck_pos].tag()))
    //   {
    //     /// xpath and deck element are equal
    //   }
    // }
    constexpr xml_attr<xpath_size> operator[](std::size_t ndx) const { return data[ndx]; }
    constexpr xml_attr<xpath_size> operator[](const cstr_t ndx) const
    {
      for (const auto& el : data)
        if (el.name() == ndx) return el;
      throw compile_error(fmt::format("unknown path '{}'.", ndx).data());
    }
    constexpr auto                      begin() const { return data.begin(); }
    constexpr auto                      end() const { return data.end(); }
    [[nodiscard]] constexpr std::size_t size() const { return paths_size; }
  private:
    std::array<xml_attr<xpath_size>, paths_size> data;
    std::array<path_node, path_node_deck_size>   deck{};
    std::size_t                                  first_free_deck_ = 0; // index of the first free deck element
    //    std::array<path_node, path_node_size>        nodes;
  };
  //....................................................................................
  template <const auto& raw_paths, const auto& ns_arr>
  consteval auto build()
  //  { return path_node_struct<raw_paths.size(), raw_paths.get_max_xpath_len()>(raw_paths, ns_arr); }
  { return path_node_struct<raw_paths>(raw_paths, ns_arr); }

} // namespace fsp
