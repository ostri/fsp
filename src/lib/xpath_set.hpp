#pragma once

#include "xml_attr.hpp"
#include "common.hpp"
#include "compile_error.hpp"
#include <array>
#include <climits>
#include <fmt/format.h>
#include <span>
namespace fsp
{
  /**
   * @brief set of xpaths to search in the xml segment
   *
   */
  class xpath_set
  {
    struct min_max
    {
      cstr_t min; // minimal xpath element tag value
      cstr_t max; // maximum xpath element tag value
    };
    using xpath_min_max = std::array<min_max, max_xpath_len>;
  public:
    static constexpr std::size_t xpath_max = 64;

    constexpr xpath_set() = default;
    constexpr xpath_set(std::span<const raw_attr> inputs, std::span<const ns> ns_arr);

    [[nodiscard]] constexpr const xml_attr& operator[](std::size_t ndx) const;
    [[nodiscard]] constexpr const xml_attr& operator[](cstr_t name) const;

    [[nodiscard]] constexpr auto        begin() const { return data_.begin(); }
    [[nodiscard]] constexpr auto        end() const;
    [[nodiscard]] constexpr std::size_t size() const { return size_; }
    [[nodiscard]] constexpr std::size_t max_xpath_size() const;

    [[nodiscard]] constexpr std::size_t last(std::size_t depth) const;
    [[nodiscard]] constexpr std::size_t first(std::size_t depth) const;

    // FIX 5: uint64_t namesto std::bitset (constexpr v C++20; bitset je constexpr šele v C++23)
    [[nodiscard]] constexpr std::uint64_t available(std::size_t depth) const;

    // dump() ni constexpr — fmt::format alokira
    [[nodiscard]] std::string dump(int offs = 0) const;

    // reserve() je bil samo za vector — ni več potreben, a ga obdržimo za kompatibilnost
    constexpr void                 reserve(std::size_t /*size*/) { }
    [[nodiscard]] constexpr cstr_t max(std::size_t depth) const;
    [[nodiscard]] constexpr cstr_t min(std::size_t depth) const;
  private:
    std::array<xml_attr, xpath_max> data_{};
    //    xpath_min_max                   mm_; // min max for each xpath element
    std::size_t size_           = 0;
    std::size_t max_xpath_size_ = 0;
  };

  // --- xpath_set impl ---------------------------------------------------------
  constexpr xpath_set::xpath_set(std::span<const raw_attr> inputs, std::span<const ns> ns_arr)
  {
    if (inputs.size() > xpath_max) throw compile_error("inputs exceed xpath_max");
    std::size_t max_d = 0;
    for (const auto& input : inputs)
    {
      xml_attr attr(size_, input, ns_arr);
      max_d = std::max(max_d, attr.size());
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      data_[size_++] = attr;
    }
    max_xpath_size_ = max_d;
    // data_[0].xpath()[0].tag = "xxx";
  }

  [[nodiscard]] constexpr const xml_attr& xpath_set::operator[](std::size_t ndx) const
  {
    if (ndx >= size_) throw compile_error("index out of range");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return data_[ndx];
  }

  [[nodiscard]] constexpr const xml_attr& xpath_set::operator[](cstr_t name) const
  {
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (data_[i].name() == name) return data_[i];
    // FIX 4: fmt::format ni constexpr — string literal zadostuje za compile_error
    throw compile_error("unknown path name");
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  [[nodiscard]] constexpr auto xpath_set::end() const { return data_.begin() + static_cast<std::ptrdiff_t>(size_); }

  [[nodiscard]] constexpr std::size_t xpath_set::max_xpath_size() const { return max_xpath_size_; }

  [[nodiscard]] constexpr std::size_t xpath_set::last(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    for (std::size_t i = size_; i-- > 0;)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) return i;
    throw compile_error("no element at depth");
  }

  [[nodiscard]] constexpr std::size_t xpath_set::first(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) return i;
    throw compile_error("no element at depth");
  }
  [[nodiscard]] constexpr cstr_t xpath_set::max(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    auto   first_ndx = first(depth);
    auto   last_ndx  = last(depth);
    cstr_t val       = data_.at(first_ndx).xpath()[depth].tag;
    for (std::size_t i = first_ndx + 1; i < last_ndx + 1; i++)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size() && data_.at(i).xpath()[depth].tag > val) //
        val = data_.at(i).xpath()[depth].tag;
    return val;
  }

  [[nodiscard]] constexpr cstr_t xpath_set::min(std::size_t depth) const
  {
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");
    auto   first_ndx = first(depth);
    auto   last_ndx  = last(depth);
    cstr_t val       = data_.at(first_ndx).xpath()[depth].tag;
    for (std::size_t i = first_ndx + 1; i < last_ndx + 1; i++)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size() && data_.at(i).xpath()[depth].tag < val) //
        val = data_.at(i).xpath()[depth].tag;
    return val;
  }

  // FIX 5: std::bitset → std::uint64_t (constexpr v C++20)
  // Bit i je postavljen, če element i obstaja na globini depth.
  // Omejitev: deluje za do 64 elementov (xpath_max = 64).
  [[nodiscard]] constexpr std::uint64_t xpath_set::available(std::size_t depth) const
  {
    static_assert(xpath_max <= sizeof(uint64_t) * CHAR_BIT, "available() uses uint64_t — xpath_max must be <= 64");
    if (depth >= max_xpath_size_) throw compile_error("depth exceeds max xpath depth");

    std::uint64_t result = 0;
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (depth < data_[i].size()) result |= (std::uint64_t{1} << i);
    return result;
  }

  // dump() ni constexpr (fmt::format alokira) — definicija brez constexpr
  [[nodiscard]] inline std::string xpath_set::dump(int offs) const
  {
    std::string msg_el;
    for (std::size_t i = 0; i < size_; ++i)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      msg_el += fmt::format("{}\n", data_[i].dump());
    return fmt::format("{}data.size: {} max_path_size: {}\n{}", std::string(offs, ' '), size_, max_xpath_size_, msg_el);
  }
} // namespace fsp