#pragma once

#include "xpath_el.hpp"
#include "xml_attr.hpp"

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


  // Span type for raw inputs


  // --- main structure (non-templated) -------------------------------------------------
  class xpath_node_struct
  {
  public:
    xpath_node_struct() = default;
    constexpr xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr const xml_attr& operator[](std::size_t ndx) const;
    [[nodiscard]] constexpr const xml_attr& operator[](cstr_t name) const;

    [[nodiscard]] constexpr auto        begin() const { return data_.begin(); }
    [[nodiscard]] constexpr auto        end() const { return data_.end(); }
    [[nodiscard]] constexpr std::size_t size() const { return data_.size(); }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const;

    [[nodiscard]] constexpr std::pair<cstr_t, std::size_t> last_xpath_tag_name(std::size_t depth) const;
    [[nodiscard]] constexpr std::pair<cstr_t, std::size_t> first_xpath_tag_name(std::size_t depth) const;
    [[nodiscard]] constexpr std::string                    dump(int offs) const;
    constexpr void                                         reserve(std::size_t size) { data_.reserve(size); }
  private:
    std::vector<xml_attr> data_;
    std::size_t           max_xpath_size_ = 0;
  };
  struct proc_data
  {
    fsp::xpath_node_struct              targets; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<fsp::xpath_node_struct> xpaths;  // NOLINT(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::string           dump(int offs) const
    {
      std::string msg;
      msg = fmt::format("{0}targets:{1}\n{0}xpaths.size:{2}", std::string(offs, ' '), targets.dump(offs), xpaths.size());
      return msg;
    }
  };


  ///////////////////////////////////////////////////////////////////////////////////////////////////////
  // Build function
  [[nodiscard]] constexpr xpath_node_struct build(std::span<const raw_attr> raw_paths, std::span<const ns> ns_arr) //
  { return {raw_paths, ns_arr}; }
  // xml_attr
  constexpr xml_attr::xml_attr(std::size_t original_ndx, const raw_attr& raw, std::span<const ns> ns_arr)
  : name_(raw.name)
  , path_(trim_xpath(raw.path))
  , is_opt_(raw.is_opt)
  , xpath_(parse_xpath_to_elements(path_, ns_arr))
  , xpath_size_(xpath_.size())
  , original_ndx_(original_ndx)
  , normalized_path_(this->full_xpath_with_uri())
  {
    auto& el = xpath_.back();
    if (el.tag.starts_with('@'))
    {
      attr_ = xpath_el{.ns = el.ns, .tag = el.tag.substr(1)};
      xpath_.pop_back();
    }
    else if (el.tag.starts_with('*'))
    {
      is_array_     = true;
      xpath_.back() = xpath_el{.ns = el.ns, .tag = el.tag.substr(1)};
    }
  }


  constexpr std::string xml_attr::full_xpath() const
  {
    std::string tmp;
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}", xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}", attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }

  [[nodiscard]] constexpr std::string xml_attr::full_xpath_with_uri() const
  {
    std::string tmp;
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}:{}", xpath_[i].ns, xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}:{}", attr_uri(), attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }

  constexpr cstr_t xml_attr::uri_from_prefix(cstr_t prefix, std::span<const ns> ns_arr)
  {
    for (const auto& el : ns_arr)
      if (el.prefix == prefix) return el.uri;
    std::string msg;
    for (const auto& el : ns_arr) { msg += fmt::format("prefix:'{}'\turi:'{}'\n", el.prefix, el.uri); }
    throw std::runtime_error(fmt::format( //
      "Prefix '{}' has no matching definition in ns structure.\n{}",
      prefix,
      msg));
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
      result.push_back(element);
    }
    return result;
  }

  [[nodiscard]] constexpr std::string xml_attr::dump(int offs) const
  {
    auto msg = fmt::format(                                                                                    //
      "{}name: {:15} path: {:40} is_array: {:5} is_opt {:5} attr: {}:{:15} xpath size:{:2} original ndx:{:2}", //
      std::string(offs, ' '),
      name_,
      path_,
      is_array_,
      is_opt_,
      attr_.ns,
      attr_.tag,
      xpath_size_,
      original_ndx_);
    return msg;
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
  constexpr xpath_node_struct::xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
  {
    data_.reserve(inputs.size());
    std::size_t max_d = 0;

    auto ndx = 0U;
    for (const auto& input : inputs)
    {
      xml_attr attr(ndx++, input, ns_arr);
      max_d = std::max(max_d, attr.xpath_size());
      data_.push_back(std::move(attr));
    }

    max_xpath_size_ = max_d;

    std::ranges::sort(data_, [](const xml_attr& a, const xml_attr& b) { return a.path() < b.path(); });
  }

  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](std::size_t ndx) const { return data_.at(ndx); }

  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](cstr_t name) const
  {
    for (const auto& el : data_)
      if (el.name() == name) return el;

    throw compile_error(fmt::format("unknown path '{}'.", name).data());
  }

  [[nodiscard]] constexpr std::size_t xpath_node_struct::max_xpath_size() const { return max_xpath_size_; }

  [[nodiscard]] constexpr std::pair<cstr_t, std::size_t> xpath_node_struct::last_xpath_tag_name(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());

    std::pair<cstr_t, std::size_t> res{};
    for (const auto& [ndx, el] : std::views::enumerate(data_))
    {
      if (depth >= el.xpath_size()) continue;
      auto val = el.xpath()[depth].tag;
      if (res.first.empty() || val > res.first) res = {val, ndx};
    }
    return res;
  }

  [[nodiscard]] constexpr std::pair<cstr_t, std::size_t> xpath_node_struct::first_xpath_tag_name(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());

    std::pair<cstr_t, std::size_t> res{};
    for (const auto& [ndx, el] : std::views::enumerate(data_))
    {
      if (depth >= el.xpath_size()) continue;
      auto val = el.xpath()[depth].tag;
      if (val.empty()) continue; // we are looking for minimum value but empty
      if (res.first.empty() || val < res.first) res = {val, ndx};
    }
    return res;
  }

  [[nodiscard]] constexpr std::string xpath_node_struct::dump(int offs) const
  {
    std::string msg;
    msg = fmt::format("{}data.size; {} max_path_size: {}", std::string(offs, ' '), data_.size(), max_xpath_size_);
    return msg;
  }
} // namespace fsp