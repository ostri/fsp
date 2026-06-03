#pragma once

#include "xpath_el.hpp"
#include <cstddef>
#include <span>
#include <string>
#include <vector>
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
    cstr_t name;           // name of the value
    cstr_t path;           // xml path
    bool   is_opt = false; // is optional?
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };
  using raw_inputs = std::span<const raw_attr>;
  using xpath_vec  = std::vector<xpath_el>;
  using xpath_span = std::span<const xpath_el>;
  // --- xml attribute (runtime friendly, built at compile time) ------------------------
  struct xml_attr
  {
  public:
    constexpr xml_attr() = default;
    constexpr xml_attr(std::size_t original_ndx, const raw_attr& raw, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr std::string full_xpath() const;
    [[nodiscard]] constexpr std::string full_xpath_with_uri() const;

    [[nodiscard]] constexpr cstr_t      name() const { return name_; }
    [[nodiscard]] constexpr cstr_t      path() const { return path_; }
    [[nodiscard]] constexpr bool        is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool        is_attr() const { return ! attr_.tag.empty(); }
    [[nodiscard]] constexpr bool        is_array() const { return is_array_; }
    [[nodiscard]] constexpr xpath_span  xpath() const { return xpath_; }
    [[nodiscard]] constexpr std::size_t xpath_size() const { return xpath_.size(); }
    [[nodiscard]] constexpr cstr_t      attr_name() const { return attr_.tag; }
    [[nodiscard]] constexpr cstr_t      attr_uri() const { return attr_.ns; }
    [[nodiscard]] constexpr xpath_el    last() const { return xpath_.back(); }
    [[nodiscard]] constexpr std::size_t original_ndx() const { return original_ndx_; }
    [[nodiscard]] constexpr std::string dump(int offs = 0) const;
  private:
    static constexpr cstr_t    trim_xpath(cstr_t str);
    static constexpr cstr_t    uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr);
    static constexpr xpath_vec parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr);
  private:
    cstr_t      name_;
    cstr_t      path_;
    bool        is_array_ = false;
    bool        is_opt_   = false;
    xpath_vec   xpath_;
    xpath_el    attr_;
    std::size_t xpath_size_   = 0; //
    std::size_t original_ndx_ = 0; // original index before sorting to connect with child structure
    std::string normalized_path_;  // all prefixes are expanded with uri and attribute and array marks added
  };
}; // namespace fsp