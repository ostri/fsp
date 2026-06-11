#pragma once

#include "compile_error.hpp"
#include "xpath_el.hpp"
#include "xml_attr.hpp"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <chrono>
#include <fmt/format.h>
#include <ranges>
#include <string_view>
#include <vector>
#include <span>
// #include <exception>
#include <string>
#include "static_xpath_vec.hpp"

namespace fsp
{
  using date_t = std::chrono::year_month_day;
  using cstr_t = std::string_view;


  // --- namespace definition -----------------------------------------------------------


  // Span type for raw inputs


  // --- main structure (non-templated) -------------------------------------------------
  class xpath_node_struct
  {
  public:
    static constexpr const int xpath_max = 64;
    xpath_node_struct()                  = default;
    constexpr xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr const xml_attr& operator[](std::size_t ndx) const;
    [[nodiscard]] constexpr const xml_attr& operator[](cstr_t name) const;

    [[nodiscard]] constexpr auto        begin() const { return data_.begin(); }
    [[nodiscard]] constexpr auto        end() const { return data_.end(); }
    [[nodiscard]] constexpr std::size_t size() const { return data_.size(); }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const;

    [[nodiscard]] constexpr std::size_t                               last(std::size_t depth) const;
    [[nodiscard]] constexpr std::size_t                               first(std::size_t depth) const;
    [[nodiscard]] constexpr std::string                               dump(int offs = 0) const;
    constexpr void                                                    reserve(std::size_t size) { data_.reserve(size); }
    [[nodiscard]] constexpr std::bitset<xpath_node_struct::xpath_max> available(std::size_t depth) const;
  private:
    std::vector<xml_attr> data_;
    std::size_t           max_xpath_size_ = 0;
  };
  struct proc_data
  {
    fsp::xpath_node_struct              targets; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<fsp::xpath_node_struct> xpaths;  // NOLINT(misc-non-private-member-variables-in-classes)
    [[nodiscard]] std::string           dump(int offs = 0) const
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


  // path_node_struct
  constexpr xpath_node_struct::xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
  {
    data_.reserve(inputs.size());
    std::size_t max_d = 0;

    auto ndx = 0U;
    for (const auto& input : inputs)
    {
      xml_attr attr(ndx++, input, ns_arr);
      max_d = std::max(max_d, attr.size());
      data_.push_back(std::move(attr));
    }

    max_xpath_size_ = max_d;

    // std::ranges::sort(data_, [](const xml_attr& a, const xml_attr& b) { return a.path() < b.path(); });
  }
  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](std::size_t ndx) const { return data_.at(ndx); }
  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](cstr_t name) const
  {
    for (const auto& el : data_)
      if (el.name() == name) return el;
    throw compile_error(fmt::format("unknown path '{}'.", name).data());
  }
  [[nodiscard]] constexpr std::size_t xpath_node_struct::max_xpath_size() const { return max_xpath_size_; }
  [[nodiscard]] constexpr std::size_t xpath_node_struct::last(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());
    for (const auto& [ndx, el] : std::views::enumerate(data_) | std::views::reverse)
      if (depth < el.size()) return ndx;
    throw std::runtime_error(fmt::format("max empty depth: {}", depth));
  }

  [[nodiscard]] constexpr std::size_t xpath_node_struct::first(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());
    for (const auto& [ndx, el] : std::views::enumerate(data_))
      if (depth < el.size()) return ndx;
    throw std::runtime_error(fmt::format("min empty depth: {}", depth));
  }
  [[nodiscard]] constexpr std::bitset<xpath_node_struct::xpath_max> xpath_node_struct::available(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error(fmt::format("depth {} exceeds max xpath depth {}", depth, max_xpath_size_).data());
    assert(depth >= max_xpath_size_);
    std::bitset<xpath_max> result{};

    for (const auto& [ndx, el] : std::views::enumerate(data_))
      if (depth < el.size()) result.set(ndx);
    return result;
  }

  [[nodiscard]] constexpr std::string xpath_node_struct::dump(int offs) const
  {
    std::string msg;
    std::string msg_el;
    for (const auto& el : data_) msg_el += fmt::format("{}\n", el.dump());
    msg = fmt::format("{}data.size; {} max_path_size: {}\n{}", std::string(offs, ' '), data_.size(), max_xpath_size_, msg_el);
    return msg;
  }
} // namespace fsp