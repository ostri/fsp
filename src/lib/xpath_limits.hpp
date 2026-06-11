#pragma once

#include "parsing_util.hpp"
// #include "xml_attr.hpp"
#include <bitset>
#include <string_view>
#include <vector>
#include <cstddef>
namespace fsp
{
  using cstr_t                          = std::string_view;
  static constexpr const int max_xpaths = 64; // maximum number of xpaths provided to algorithm
                                              //  if more needed, we need to adjust algorithmf for the
                                              //  first and last bit set
  using bit_set = std::bitset<max_xpaths>;
  class p_limits
  {
  public:
    constexpr p_limits(std::size_t low_ndx, std::size_t high_ndx)
    {
      for (auto cnt = low_ndx; cnt <= high_ndx; cnt++) available_[cnt] = true;
    }
    constexpr explicit p_limits(bit_set bs)
    : available_(bs)
    {
    }
    p_limits() = default;
    p_limits& operator&=(const p_limits& rhs)
    {
      this->available_ &= rhs.available_;
      return *this;
    }
    [[nodiscard]] constexpr std::string dump(int offs = 0) const
    {
      std::string tmp = available_.to_string('.', '*') | std::views::reverse | std::ranges::to<std::string>();
      return fmt::format("{}low: {} high: {} arr: [{}]", //
                         std::string(offs, ' '),
                         first(),
                         last(),
                         tmp.substr(first(), last() - first() + 1));
    }
    [[nodiscard]] constexpr std::size_t first() const
    {
      const auto val = available_.to_ullong();
      if (val != 0) return __builtin_ctz(available_.to_ullong());
      return available_.size();
    }
    [[nodiscard]] constexpr std::size_t last() const
    {
      const int bits_in_byte = 8;
      auto      val          = available_.to_ullong();
      if (val != 0) return (sizeof(val) * bits_in_byte) - __builtin_clzll(val) - 1;
      return available_.size();
    }
    void                            reset(std::size_t ndx) { available_.reset(ndx); }
    void                            set(std::size_t ndx) { available_.set(ndx); }
    std::size_t                     count() { return available_.count(); }
    [[nodiscard]] constexpr bit_set available() const { return available_; };
    friend constexpr p_limits       operator&(p_limits lhs, const p_limits& rhs)
    {
      lhs &= rhs;
      return lhs;
    }
  private:
    bit_set available_; // list of available paths
  };

  class xpath_limits
  {
  public:
    explicit xpath_limits(const xpath_node_struct& xpaths)
    {
      data_.reserve(xpaths.max_xpath_size());
      for (auto cnt = 0UL; cnt < xpaths.max_xpath_size(); cnt++)
      {
        auto low  = xpaths.first(cnt);
        auto high = xpaths.last(cnt);
        auto tmp  = p_limits(low, high);
        data_.push_back(tmp);
      }
    }
    [[nodiscard]] constexpr const p_limits&    operator[](std::size_t ndx) const { return data_[ndx]; };
    [[nodiscard]] p_limits&                    operator[](std::size_t ndx) { return data_[ndx]; };
    [[nodiscard]] std::size_t                  size() const { return data_.size(); }
    [[nodiscard]] const std::vector<p_limits>& limits() const { return data_; }
    [[nodiscard]] std::string                  dump(int offs = 0) const
    {
      std::string msg = fmt::format("{}Limits:\n", std::string(offs, ' '));
      for (const auto& [cnt, el] : std::views::enumerate(data_))
        msg += fmt::format("{}- [{}] {}\n", //
                           std::string(offs, ' '),
                           cnt,
                           el.dump());
      return fmt::format("\n{}", msg);
    }
  private:
    std::vector<p_limits> data_;
  };
} // namespace fsp