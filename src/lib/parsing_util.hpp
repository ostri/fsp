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
  // --- compile time exception ----------------------------------------------------------
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
  // --- prefix / uri definition ---------------------------------------------------------
  struct ns
  {
    cstr_t prefix;
    cstr_t uri;
  };
  // --- xpath element -------------------------------------------------------------------
  struct xpath_el
  {
    cstr_t ns;
    cstr_t tag;
  };
  ////////////////////////////////////////////////////////////////////////////////////////
  // --- raw xpath definition (name, xpath (attr/array), optional) -----------------------
  // public attributes and no contructor to be able to make a constant definition in code
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
  ///////////////////////////////////////////////////////////////////////////////////////
  // --- setof raw_attr elements
  // dimension calculated in compile time -> std::array
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

  // deduction guide to get the dimension from the structure not from explicit value
  template <std::size_t N>
  raw_inputs_container(std::array<fsp::raw_attr, N>) -> raw_inputs_container<N>;
  //////////////////////////////////////////////////////////////////////////////////


  consteval cstr_t uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
      if (el.prefix == prefix) return el.uri;
    throw compile_error("Prefix has no matching definition in ns structure.");
  }

  template <std::size_t xpath_len>
  struct xml_attr
  {
  public:
    constexpr xml_attr() = default;
    constexpr xml_attr(raw_attr raw, const auto& ns_arr)
    : name_(raw.name)
    , path_(trim_xpath(raw.path)) // remove trailing & leading whitespaces and '/'
    , is_opt_(raw.is_opt)
    {
      auto tmp    = parse_xpath_to_elements(raw.path, ns_arr); // NOLINT(cppcoreguidelines-prefer-member-initializer)
      xpath_size_ = tmp.size();
      for (auto cnt = 0U; cnt < xpath_size_; cnt++)
      {
        if (tmp[cnt].tag.starts_with('@'))
        {
          xpath_size_--;                                                      // attribute is removed from the xpath
          attr_ = xpath_el{.ns = tmp[cnt].ns, .tag = tmp[cnt].tag.substr(1)}; // strip leading character (@)
          continue;
        }
        if (tmp[cnt].tag.starts_with('*'))
        {
          is_array_   = true;
          xpath_[cnt] = xpath_el{.ns = tmp[cnt].ns, .tag = tmp[cnt].tag.substr(1)};
          continue;
        }
        xpath_[cnt] = tmp[cnt];
      }
    }
    static consteval std::vector<xpath_el> parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr)
    {
      // cstr_t trimmed = trim_xpath(input); // trim leading and trailing whitespaces and /
      // if (trimmed.empty()) return {};     // no xpath no work
      if (input.empty()) throw compile_error(fmt::format("empty xpath").data());
      std::vector<xpath_el> result;
      for (auto segment_range : input | std::views::split('/')) // split by /
      {
        cstr_t segment{segment_range};
        if (segment.empty()) continue; // if xpath contains multiple // in sequence
        xpath_el element{};
        auto     colon = segment.find(':'); // split by colon
        if (colon != cstr_t::npos)
        { // ns + tag -> just split and calculate uri
          element.ns  = uri_from_prefix(segment.substr(0, colon), ns_arr);
          element.tag = segment.substr(colon + 1);
        }
        else
        { // no prefix: attribue does not inherit default namespace, tag does -> remove prefix
          element.ns  = segment.starts_with('@') ? "" : uri_from_prefix("", ns_arr);
          element.tag = segment;
        }
        result.push_back(element);
      }
      return result;
    }
    // Pomožna funkcija za odstranjevanje presledkov (White Spaces) in poševnic
    static constexpr cstr_t trim_xpath(cstr_t str)
    {
      auto        is_ws_or_slash = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/'; };
      const auto* start          = std::ranges::find_if_not(str, is_ws_or_slash);
      if (start == str.end())
        throw compile_error(fmt::format("Empty xpath after trimming (first). '{}'", str).data()); // empty xpath after left trimming
      const auto* end = std::ranges::find_if_not(str | std::views::reverse, is_ws_or_slash).base();
      return {start, end};
    }
    [[nodiscard]] constexpr std::string full_xpath() const
    {
      std::string tmp;
      for (auto cnt = 0U; cnt < xpath_size_; cnt++) tmp += fmt::format("[{}]", xpath_[cnt].tag);
      if (is_attr()) tmp += fmt::format("[{}]@", attr_name());
      else if (is_array()) tmp += fmt::format("*");
      return tmp;
    }
    [[nodiscard]] constexpr std::string full_xpath_with_uri() const
    {
      std::string tmp;
      for (auto cnt = 0U; cnt < xpath_size_; cnt++) tmp += fmt::format("[{}:{}]", xpath_[cnt].ns, xpath_[cnt].tag);
      if (is_attr()) tmp += fmt::format("[{}:{}]@", attr_uri(), attr_name());
      else if (is_array()) tmp += fmt::format("*");
      return tmp;
    }
    [[nodiscard]] constexpr cstr_t      name() const { return name_; }
    [[nodiscard]] constexpr cstr_t      path() const { return path_; }
    [[nodiscard]] constexpr bool        is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool        is_attr() const { return ! attr_.tag.empty(); }
    [[nodiscard]] constexpr bool        is_array() const { return is_array_; }
    [[nodiscard]] constexpr auto        xpath() const { return xpath_; }
    [[nodiscard]] constexpr auto&       xpath() { return xpath_; }
    [[nodiscard]] constexpr std::size_t xpath_size() const { return xpath_size_; }
    [[nodiscard]] constexpr xpath_el&   last() { return xpath_.at(xpath_size() - 1); }
    [[nodiscard]] constexpr cstr_t      attr_name() const { return attr_.tag; }
    [[nodiscard]] constexpr cstr_t      attr_uri() const { return attr_.ns; }
  private:
    cstr_t                          name_;
    cstr_t                          path_;
    bool                            is_array_ = false;
    bool                            is_opt_   = false;
    std::array<xpath_el, xpath_len> xpath_{};
    xpath_el                        attr_;           // if it is attribute path it holds a name of the attribute
    std::size_t                     xpath_size_ = 0; // size of the xpath (number of nonempty elements)
  };

  using date_t = std::chrono::year_month_day;

  // consteval std::size_t get_xpaths_max_depth(std::span<const raw_attr> attrs)
  // {
  //   std::size_t max_d = 0;
  //   for (const auto& attr : attrs) { max_d = std::max(max_d, attr.xpath_size()); }
  //   return max_d;
  // }

  // class path_node
  // {
  // public:
  //   static constexpr const auto max_int = std::numeric_limits<std::size_t>::max();
  //   constexpr path_node()
  //   : fsp::path_node("", "")
  //   {
  //   }
  //   constexpr path_node(cstr_t uri, cstr_t tag, std::size_t xp_node, std::size_t next_xpath)
  //   : uri_(uri)
  //   , tag_(tag)
  //   , xp_node_(xp_node)
  //   , next_xpath_(next_xpath)
  //   {
  //   }
  //   constexpr path_node(cstr_t uri, cstr_t tag)
  //   : path_node(uri, tag, max_int, max_int)
  //   {
  //   }
  //   [[nodiscard]] constexpr std::size_t xp_node() const { return xp_node_; }
  //   [[nodiscard]] constexpr std::size_t next_xpath() const { return next_xpath_; }
  //   [[nodiscard]] constexpr bool        is_xpath_last_node() const { return xp_node_ == max_int; }
  // private:
  //   cstr_t      uri_;                  // tag's uri
  //   cstr_t      tag_;                  // tag name
  //   std::size_t xp_node_    = max_int; // xpath node or max_int if this is the end of the xpath
  //   std::size_t next_xpath_ = max_int; // next xpath
  // };
  // template </*std::size_t path_node_size,*/ std::size_t paths_size, std::size_t xpath_size>
  template <const auto& ra>
  class path_node_struct
  {
  public:
    static constexpr const auto MAX_XPATH_SIZE      = ra.get_max_xpath_len();
    static constexpr const auto PATHS_SIZE          = ra.size();
    static constexpr const auto path_node_deck_size = ra.get_total_xpath_len();
    consteval path_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
    {
      for (std::size_t i = 0; i < inputs.size(); ++i)
      {
        auto tmp      = xml_attr<MAX_XPATH_SIZE>(inputs[i], ns_arr);
        xpath_dscr[i] = tmp;
        // push_xpath(tmp.xpath().begin(), deck, 0);
      }
    }
    //    consteval std::size_t get_new_node() { return first_free_deck_++; }
    // constexpr std::size_t push_xpath(const auto& xpath_el, auto& deck, std::size_t deck_pos)
    // {
    //   if ((xpath_el.uri() == deck[deck_pos].uri()) && (xpath_el.tag() == deck[deck_pos].tag()))
    //   {
    //     /// xpath and deck element are equal
    //   }
    // }
    constexpr xml_attr<MAX_XPATH_SIZE> operator[](std::size_t ndx) const { return xpath_dscr[ndx]; }
    constexpr xml_attr<MAX_XPATH_SIZE> operator[](const cstr_t ndx) const
    {
      for (const auto& el : xpath_dscr)
        if (el.name() == ndx) return el;
      throw compile_error(fmt::format("unknown path '{}'.", ndx).data());
    }
    constexpr auto                      begin() const { return xpath_dscr.begin(); }
    constexpr auto                      end() const { return xpath_dscr.end(); }
    [[nodiscard]] constexpr std::size_t size() const { return PATHS_SIZE; }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const { return MAX_XPATH_SIZE; }

    /*!
     * The method returns the value of the xpath tag, that has the lowest value in the list of
     * provided xpaths on the provided depth
     * E.g.
     * a/b/c
     * a/b/d
     * c/d/e/f
     * first_xpath_tag_name(0) -> a
     * first_xpath_tag_name(1) -> b
     * first_xpath_tag_name(2) -> c
     * first_xpath_tag_name(3) -> f
     * first_xpath_tag_name(4) -> empty
     */
    [[nodiscard]] constexpr cstr_t first_xpath_tag_name(std::size_t depth) const
    {
      static_assert(MAX_XPATH_SIZE != 0, "No xpath definitions. There must be at leas one xpath definition provided.");
      if (depth >= MAX_XPATH_SIZE) //
        throw compile_error(fmt::format("too deep. depth:{} max depth: {}", depth, MAX_XPATH_SIZE).data());
      cstr_t res;
      for (const auto& el : xpath_dscr) //
      {
        auto val = el.xpath()[depth].tag;
        if (res.empty() && ! val.empty())
        {
          res = val; // we found a nonempty tag in xpath on the provided depth
          continue;
        };
        if (! val.empty() && val < res) res = val; // we found smaller
      }
      return res;
    }
    [[nodiscard]] constexpr cstr_t last_xpath_tag_name(std::size_t depth) const
    {
      static_assert(MAX_XPATH_SIZE != 0, "No xpath definitions. There must be at leas one xpath definition provided.");
      if (depth >= MAX_XPATH_SIZE) //
        throw compile_error(fmt::format("too deep. depth:{} max depth: {}", depth, MAX_XPATH_SIZE).data());
      cstr_t res;
      for (const auto& el : xpath_dscr) //
      {
        auto val = el.xpath()[depth].tag;
        if (res.empty() && ! val.empty())
        {
          res = val; // we found a nonempty tag in xpath on the provided depth
          continue;
        };
        if (! val.empty() && val > res) res = val; // we found bigger (testing empty is redundant)
      }
      return res;
    }
  private:
    std::array<xml_attr<MAX_XPATH_SIZE>, PATHS_SIZE> xpath_dscr;
    //    std::array<path_node, path_node_deck_size>   deck{};
    // std::size_t first_free_deck_ = 0; // index of the first free deck element
    //    std::array<path_node, path_node_size>        nodes;
  };
  //....................................................................................
  template <const auto& raw_paths, const auto& ns_arr>
  consteval auto build()
  { return path_node_struct<raw_paths>(raw_paths, ns_arr); }

} // namespace fsp
