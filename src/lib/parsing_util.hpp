#pragma once

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <ranges>
#include <string_view>
#include <vector>
#include <span>
#include <exception>
#include <string>
#include "static_xpath_vec.hpp"

namespace fsp
{
  using date_t = std::chrono::year_month_day;
  using cstr_t = std::string_view;


  // --- compile time exception ----------------------------------------------------------
  class compile_error : public std::exception
  {
  public:
    // compile_error
    constexpr explicit compile_error(const char* msg) noexcept
    : message(msg)
    {
    }
    [[nodiscard]] constexpr const char* what() const noexcept override { return message; }
  private:
    const char* message;
  };

  // --- namespace definition -----------------------------------------------------------
  struct ns
  {
    cstr_t prefix;
    cstr_t uri;
  };

  // --- raw attribute definition (compile-time) ----------------------------------------
  struct raw_attr
  {
    // raw_attr
    [[nodiscard]] consteval std::size_t        xpath_size() const { return xpath_size(path); }
    [[nodiscard]] static consteval std::size_t xpath_size(cstr_t path)
    {
      char ch = '/';
      if (path.empty()) return 0;
      if (path[0] == ch) return std::ranges::count(path.substr(1), ch) + 1;
      return std::ranges::count(path, ch) + 1;
    }

    bool operator==(const raw_attr& o) const { return (o.name == name) && (o.path == path); }
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t name;           // name of the value
    cstr_t path;           // xml path
    bool   is_opt = false; // is optional?
    // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  // Span type for raw inputs
  using raw_inputs = std::span<const raw_attr>;
  using xpath_vec  = static_xpath_vec; // std::vector<xpath_el>;
  using xpath_span = std::span<const xpath_el>;

  // --- xml attribute (runtime friendly, built at compile time) ------------------------
  struct xml_attr
  {
  public:
    constexpr xml_attr() = default;
    constexpr xml_attr(const raw_attr& raw, std::span<const ns> ns_arr);

    [[nodiscard]] std::string full_xpath() const;
    [[nodiscard]] std::string full_xpath_with_uri() const
    {
      std::string tmp;
      for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}:{}", xpath_[i].ns, xpath_[i].tag);

      if (is_attr()) tmp += fmt::format("/@{}:{}", attr_uri(), attr_name());
      else if (is_array()) tmp += "/*";

      return tmp;
    }

    [[nodiscard]] constexpr cstr_t      name() const { return name_; }
    [[nodiscard]] constexpr cstr_t      path() const { return path_; }
    static constexpr cstr_t             uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr);
    static constexpr xpath_vec          parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr);
    [[nodiscard]] constexpr bool        is_opt() const { return is_opt_; }
    [[nodiscard]] constexpr bool        is_attr() const { return ! attr_.tag.empty(); }
    [[nodiscard]] constexpr bool        is_array() const { return is_array_; }
    [[nodiscard]] constexpr xpath_span  xpath() const { return xpath_; }
    [[nodiscard]] constexpr std::size_t xpath_size() const { return xpath_.size(); }
    [[nodiscard]] constexpr cstr_t      attr_name() const { return attr_.tag; }
    [[nodiscard]] constexpr cstr_t      attr_uri() const { return attr_.ns; }
    [[nodiscard]] constexpr auto        last() const { return xpath_[xpath_.size() - 1]; }
  private:
    static constexpr cstr_t trim_xpath(cstr_t str);
  private:
    cstr_t      name_;
    cstr_t      path_;
    bool        is_array_ = false;
    bool        is_opt_   = false;
    xpath_vec   xpath_;
    xpath_el    attr_;
    std::size_t xpath_size_ = 0;
  };

  // --- main structure (non-templated) -------------------------------------------------
  class xpath_node_struct
  {
  public:
    constexpr xpath_node_struct(raw_inputs inputs, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr const xml_attr& operator[](std::size_t ndx) const { return data_.at(ndx); }
    [[nodiscard]] constexpr const xml_attr& operator[](cstr_t name) const;

    [[nodiscard]] constexpr auto        begin() const { return data_.begin(); }
    [[nodiscard]] constexpr auto        end() const { return data_.end(); }
    [[nodiscard]] constexpr std::size_t size() const { return data_.size(); }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const { return max_xpath_size_; }

    [[nodiscard]] constexpr cstr_t last_xpath_tag_name(std::size_t depth) const;
    [[nodiscard]] constexpr cstr_t first_xpath_tag_name(std::size_t depth) const;
  private:
    std::vector<xml_attr> data_;
    std::size_t           max_xpath_size_ = 0;
  };
  ///////////////////////////////////////////////////////////////////////////////////////////////////////
  // Build function
  [[nodiscard]] constexpr xpath_node_struct build(raw_inputs raw_paths, std::span<const ns> ns_arr) //
  { return {raw_paths, ns_arr}; }
  // xml_attr
  constexpr xml_attr::xml_attr(const raw_attr& raw, std::span<const ns> ns_arr)
  : name_(raw.name)
  , path_(trim_xpath(raw.path))
  , is_opt_(raw.is_opt)
  {
    auto tmp    = parse_xpath_to_elements(raw.path, ns_arr);
    xpath_size_ = tmp.size();
    // xpath_.reserve(tmp.size());

    for (const auto& el : tmp)
    {
      if (el.tag.starts_with('@'))
      {
        xpath_size_--;
        attr_ = xpath_el{.ns = el.ns, .tag = el.tag.substr(1)};
        continue;
      }
      if (el.tag.starts_with('*'))
      {
        is_array_ = true;
        xpath_.emplace_back(xpath_el{.ns = el.ns, .tag = el.tag.substr(1)});
        continue;
      }
      xpath_.push_back(el);
    }
  }

  inline std::string xml_attr::full_xpath() const
  {
    std::string tmp;
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}", xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}", attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }

  constexpr cstr_t xml_attr::uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
      if (el.prefix == prefix) return el.uri;
    throw compile_error("Prefix has no matching definition in ns structure.");
  }

  constexpr xpath_vec xml_attr::parse_xpath_to_elements(cstr_t input, std::span<const ns> ns_arr)
  {
    if (input.empty()) throw compile_error("empty xpath");

    xpath_vec result;

    for (auto segment_range : input | std::ranges::views::split('/'))
    {
      cstr_t segment{segment_range};
      if (segment.empty()) continue;

      xpath_el element{};
      auto     colon = segment.find(':');

      if (colon != cstr_t::npos)
      {
        element.ns  = uri_from_prefix(segment.substr(0, colon), ns_arr);
        element.tag = segment.substr(colon + 1);
      }
      else
      {
        element.ns  = segment.starts_with('@') ? "" : uri_from_prefix("", ns_arr);
        element.tag = segment;
      }
      result.push_back(element);
    }
    return result;
  }

  constexpr cstr_t xml_attr::trim_xpath(cstr_t str)
  {
    auto is_ws_or_slash = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/'; };

    const auto* start = std::ranges::find_if_not(str, is_ws_or_slash);
    if (start == str.end()) throw compile_error(fmt::format("Empty xpath after trimming: '{}'", str).data());

    const auto* end = std::ranges::find_if_not(str | std::views::reverse, is_ws_or_slash).base();
    return {start, end};
  }

  // path_node_struct
  constexpr xpath_node_struct::xpath_node_struct(raw_inputs inputs, std::span<const ns> ns_arr)
  {
    data_.reserve(inputs.size());
    std::size_t max_d = 0;

    for (const auto& input : inputs)
    {
      xml_attr attr(input, ns_arr);
      max_d = std::max(max_d, attr.xpath_size());
      data_.push_back(std::move(attr));
    }

    max_xpath_size_ = max_d;

    std::ranges::sort(data_, [](const xml_attr& a, const xml_attr& b) { return a.path() < b.path(); });
  }

  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](cstr_t name) const
  {
    for (const auto& el : data_)
      if (el.name() == name) return el;

    throw compile_error(fmt::format("unknown path '{}'.", name).data());
  }

  [[nodiscard]] constexpr cstr_t xpath_node_struct::last_xpath_tag_name(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());

    cstr_t res{};
    for (const auto& el : data_)
    {
      if (depth >= el.xpath_size()) continue;
      auto val = el.xpath()[depth].tag;
      if (res.empty() || val > res) res = val;
    }
    return res;
  }

  [[nodiscard]] constexpr cstr_t xpath_node_struct::first_xpath_tag_name(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());

    cstr_t res{};
    for (const auto& el : data_)
    {
      if (depth >= el.xpath_size()) continue;
      auto val = el.xpath()[depth].tag;
      if (val.empty()) continue; // we are looking for minimum value but empty
      if (res.empty() || val < res) res = val;
    }
    return res;
  }

} // namespace fsp