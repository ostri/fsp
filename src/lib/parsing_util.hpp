#pragma once

#include "compile_error.hpp"
#include "xpath_el.hpp"
#include "xml_attr.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fmt/format.h>
// #include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fsp
{
  using date_t = std::chrono::year_month_day;
  using cstr_t = std::string_view;

  // --- main structure -----------------------------------------------------------------
  class xpath_node_struct
  {
  public:
    static constexpr std::size_t xpath_max = 64;

    constexpr xpath_node_struct() = default;
    constexpr xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr const xml_attr& operator[](std::size_t ndx) const;
    [[nodiscard]] constexpr const xml_attr& operator[](cstr_t name) const;

    [[nodiscard]] constexpr auto begin() const { return data_.begin(); }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    [[nodiscard]] constexpr auto        end() const { return data_.begin() + static_cast<std::ptrdiff_t>(size_); }
    [[nodiscard]] constexpr std::size_t size() const { return size_; }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const { return max_xpath_size_; }

    [[nodiscard]] constexpr std::size_t last(std::size_t depth) const;
    [[nodiscard]] constexpr std::size_t first(std::size_t depth) const;

    // FIX 5: uint64_t namesto std::bitset (constexpr v C++20; bitset je constexpr šele v C++23)
    [[nodiscard]] constexpr std::uint64_t available(std::size_t depth) const;

    // dump() ni constexpr — fmt::format alokira
    [[nodiscard]] std::string dump(int offs = 0) const;

    // reserve() je bil samo za vector — ni več potreben, a ga obdržimo za kompatibilnost
    constexpr void reserve(std::size_t /*size*/) { }
  private:
    // FIX 1: std::vector → std::array + size_ (literal type, constexpr možen)
    std::array<xml_attr, xpath_max> data_{};
    std::size_t                     size_           = 0;
    std::size_t                     max_xpath_size_ = 0;
  };

  // --- proc_data ----------------------------------------------------------------------
  // Opomba: xpaths ostane std::vector — proc_data ni constexpr, je runtime struktura.
  // Če bi hoteli constexpr proc_data, bi potrebovali std::array<xpath_node_struct, N>.
  struct proc_data
  {
    fsp::xpath_node_struct              targets; // NOLINT(misc-non-private-member-variables-in-classes)
    std::vector<fsp::xpath_node_struct> xpaths;  // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] std::string dump(int offs = 0) const
    { return fmt::format("{0}targets:{1}\n{0}xpaths.size:{2}", std::string(offs, ' '), targets.dump(offs), xpaths.size()); }
  };

  // --- build --------------------------------------------------------------------------
  [[nodiscard]] constexpr xpath_node_struct build(std::span<const raw_attr> raw_paths, std::span<const ns> ns_arr)
  { return {raw_paths, ns_arr}; }

  // --- xpath_node_struct impl ---------------------------------------------------------

  // FIX 1+2: konstruktor brez vector::reserve / push_back — piše direktno v array
  constexpr xpath_node_struct::xpath_node_struct(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
  {
    if (inputs.size() > xpath_max) throw compile_error("inputs exceed xpath_max");

    std::size_t max_d = 0;
    for (const auto& input : inputs)
    {
      xml_attr attr(size_, input, ns_arr);
      max_d = std::max(max_d, attr.size());
      // FIX 2: push_back zamenjano z direktnim pisanjem v array
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      data_[size_++] = attr;
    }
    max_xpath_size_ = max_d;
  }

  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](std::size_t ndx) const
  {
    if (ndx >= size_) throw compile_error("index out of range");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return data_[ndx];
  }

  [[nodiscard]] constexpr const xml_attr& xpath_node_struct::operator[](cstr_t name) const
  {
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (data_[i].name() == name) return data_[i];
    // FIX 4: fmt::format ni constexpr — string literal zadostuje za compile_error
    throw compile_error("unknown path name");
  }

  [[nodiscard]] constexpr std::size_t xpath_node_struct::last(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    for (std::size_t i = size_; i-- > 0;)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) return i;
    throw compile_error("no element at depth");
  }

  [[nodiscard]] constexpr std::size_t xpath_node_struct::first(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) return i;
    throw compile_error("no element at depth");
  }

  // FIX 5: std::bitset → std::uint64_t (constexpr v C++20)
  // Bit i je postavljen, če element i obstaja na globini depth.
  // Omejitev: deluje za do 64 elementov (xpath_max = 64).
  [[nodiscard]] constexpr std::uint64_t xpath_node_struct::available(std::size_t depth) const
  {
    static_assert(xpath_max <= sizeof(uint64_t) * CHAR_BIT, "available() uses uint64_t — xpath_max mora biti <= 64");
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");

    std::uint64_t result = 0;
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) result |= (std::uint64_t{1} << i);
    return result;
  }

  // dump() ni constexpr (fmt::format alokira) — definicija brez constexpr
  [[nodiscard]] inline std::string xpath_node_struct::dump(int offs) const
  {
    std::string msg_el;
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      msg_el += fmt::format("{}\n", data_[i].dump());
    return fmt::format("{}data.size: {} max_path_size: {}\n{}", std::string(offs, ' '), size_, max_xpath_size_, msg_el);
  }

} // namespace fsp
