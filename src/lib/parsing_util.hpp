#pragma once

#include <array>
#include <chrono>
#include <string_view>
#include <vector>
#include <span>
#include <algorithm>
#include <limits>
#include <exception>

namespace fsp
{
  class compile_time_error : public std::exception
  {
  public: // NUJNO: ker class privzeto skrije vse pod 'private'
    constexpr explicit compile_time_error(const char* msg)
    : message(msg)
    {
    }

    [[nodiscard]] constexpr const char* what() const noexcept override { return message; }
  private:
    const char* message;
  };

  struct ns
  {
    std::string_view prefix;
    std::string_view uri;
  };

  struct xpath_el
  {
    std::string_view ns;
    std::string_view tag;
  };

  struct raw_attr
  {
  public:
    [[nodiscard]] constexpr size_t        xpath_size() const { return xpath_size(path); }
    [[nodiscard]] static constexpr size_t xpath_size(std::string_view path)
    {
      char ch = '/';
      if (path.empty()) return 0;
      if (path[0] == ch) return std::ranges::count(path.substr(1), ch) + 1;
      return std::ranges::count(path, ch) + 1;
    }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string_view name;
    std::string_view path;
    bool             is_opt = false;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  template <size_t xpath_len>
  struct xml_attr
  {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::string_view name;
    std::string_view path;
    bool             is_attr  = false;
    bool             is_array = false;
    bool             is_opt   = false;

    // Končni fiksni pomnilnik, ki ostane v constexpr objektu
    std::array<xpath_el, xpath_len> xpath{};
    size_t                          xpath_size_ = std::numeric_limits<std::size_t>::max();
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] constexpr size_t xpath_size()
    {
      if (xpath_size_ != std::numeric_limits<std::size_t>::max()) return xpath_size_;
      char ch = '/';
      if (path.empty()) xpath_size_ = 0;
      else if (path[0] == ch) xpath_size_ = std::ranges::count(path.substr(1), ch) + 1;
      else xpath_size_ = std::ranges::count(path, ch) + 1; // Popravljeno štetje na 'path'
      return xpath_size_;
    }

    [[nodiscard]] constexpr xpath_el& last() { return xpath.at(xpath_size() - 1); }
  };

  using date_t = std::chrono::year_month_day;

  consteval std::string_view uri_from_prefix(std::string_view prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
    {
      if (el.prefix == prefix) return el.uri;
    }
    throw compile_time_error("Prefix has no matching definition in ns structure.");
  }

  // Funkcija sedaj interna uporablja std::vector in vrača anonimno strukturo z vektorjem
  consteval auto parse_xpath(std::string_view path, std::span<const ns> ns_arr)
  {
    struct parse_result
    {
      std::vector<xpath_el> data;
      size_t                size = 0;
    };

    parse_result res{};
    size_t       start = 0;
    if (path.empty()) throw compile_time_error("attribute .path must not be empty");
    if (path.front() == '/') path = path.substr(1);

    while (start < path.size())
    {
      size_t           end     = path.find('/', start);
      std::string_view segment = path.substr(start, end - start);

      if (! segment.empty())
      {
        xpath_el element{};
        size_t   colon = segment.find(':');
        if (colon != std::string_view::npos)
        {
          element.ns  = uri_from_prefix(segment.substr(0, colon), ns_arr);
          element.tag = segment.substr(colon + 1);
        }
        else
        {
          element.ns  = uri_from_prefix("", ns_arr);
          element.tag = segment;
        }

        // Varno dodajanje v vektor, velikost raste dinamično
        res.data.push_back(element);
        res.size++;
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
    return res;
  }
  consteval size_t get_max_depth(std::span<const raw_attr> attrs)
  {
    size_t max_d = 0;
    for (const auto& attr : attrs) { max_d = std::max(max_d, attr.xpath_size()); }
    return max_d;
  }
  template <size_t depth>
  consteval xml_attr<depth> MA(raw_attr raw, std::span<const ns> ns_arr)
  {
    auto xp       = parse_xpath(raw.path, ns_arr);
    bool is_attr  = false;
    bool is_array = false;

    if (xp.size > 0)
    {
      auto& last_el = xp.data.at(xp.size - 1);
      if (! last_el.tag.empty() && last_el.tag.front() == '@')
      {
        is_attr     = true;
        last_el.tag = last_el.tag.substr(1);
      }
      else if (! last_el.tag.empty() && last_el.tag.front() == '*')
      {
        is_array    = true;
        last_el.tag = last_el.tag.substr(1);
      }
    }

    // Inicializacija strukture (polje xpath je privzeto prazno)
    xml_attr<depth> attr{
      .name = raw.name, .path = raw.path, .is_attr = is_attr, .is_array = is_array, .is_opt = raw.is_opt, .xpath_size_ = xp.size};

    // Prepis iz začasnega std::vector v končni std::array
    // std::copy deluje v consteval kontekstu od C++20 dalje.
    std::copy(xp.data.begin(), xp.data.end(), attr.xpath.begin());

    return attr;
  }

  template <size_t N, size_t depth>
  struct attr_tbl
  {
    consteval attr_tbl(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
    {
      for (size_t i = 0; i < N; ++i) { data[i] = MA<depth>(inputs[i], ns_arr); }
    }

    constexpr xml_attr<depth> operator[](size_t ndx) const { return data[ndx]; }
    constexpr xml_attr<depth> operator[](const std::string_view ndx) const
    {
      for (const auto& el : data)
        if (el.name == ndx) return el;
      return {};
    }
    constexpr auto                 begin() const { return data.begin(); }
    constexpr auto                 end() const { return data.end(); }
    [[nodiscard]] constexpr size_t size() const { return depth; }
  private:
    std::array<xml_attr<depth>, N> data{};
  };

  struct path_node
  {
    xpath_el node;
    int      child = -1;
    int      next  = -1;
  };

  template <size_t path_node_size, size_t paths_size, size_t xpath_size>
  class path_node_struct
  {
  public:
    consteval path_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
    {
      for (size_t i = 0; i < paths_size; ++i) { data[i] = MA<xpath_size>(inputs[i], ns_arr); }
    }
    constexpr xml_attr<xpath_size> operator[](size_t ndx) const { return data[ndx]; }
    constexpr xml_attr<xpath_size> operator[](const std::string_view ndx) const
    {
      for (const auto& el : data)
        if (el.name == ndx) return el;
      return {};
    }
    constexpr auto                 begin() const { return data.begin(); }
    constexpr auto                 end() const { return data.end(); }
    [[nodiscard]] constexpr size_t size() const { return paths_size; }
  private:
    std::array<xml_attr<xpath_size>, paths_size> data;
    std::array<path_node, path_node_size>        nodes;
  };

} // namespace fsp