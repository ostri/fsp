#pragma once

#include "parsing_util.hpp"
#include "xml_attr.hpp"
// #include <span>
#include <string_view>
#include <vector>
#include <algorithm>
#include <cstddef>
namespace fsp
{
  using cstr_t = std::string_view;
  class p_limits
  {
  public:
    p_limits(cstr_t low_tag, cstr_t low_uri, std::size_t low_ndx, cstr_t high_tag, cstr_t high_uri, std::size_t high_ndx, std::size_t max)
    : low_tag_(low_tag)
    , low_uri_(low_uri)
    , low_ndx_(low_ndx)
    , high_tag_(high_tag)
    , high_uri_(high_uri)
    , high_ndx_(high_ndx)
    , max_(max)
    , available_(max_, false)
    {
      std::fill(
        available_.begin() + static_cast<std::ptrdiff_t>(low_ndx), available_.begin() + static_cast<std::ptrdiff_t>(high_ndx), true);
    }
    [[nodiscard]] std::string dump(int offs = 0) const;
    // [[nodiscard]] std::vector<bool> get_available() const
    // {
    //   auto tmp = std::vector<bool>(max_, true);
    //   for (auto cnt = 0UL; cnt < low_ndx_; cnt++) tmp[cnt] = false;
    //   for (auto cnt = high_ndx_ + 1; cnt < max_; cnt++) tmp[cnt] = false;
    //   return tmp;
    // }
    [[nodiscard]] cstr_t            low_tag() const;
    [[nodiscard]] cstr_t            low_uri() const;
    [[nodiscard]] std::size_t       low_ndx() const;
    [[nodiscard]] cstr_t            high_tag() const;
    [[nodiscard]] cstr_t            high_uri() const;
    [[nodiscard]] std::size_t       high_ndx() const;
    [[nodiscard]] std::size_t       max() const;
    [[nodiscard]] std::vector<bool> available() const;
  private:
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    cstr_t            low_tag_;   // minimum tag value which is frst valid
    cstr_t            low_uri_;   // related uri
    std::size_t       low_ndx_;   // minimum index of the rule that is first valid
    cstr_t            high_tag_;  // maximum tag value which is still valid
    cstr_t            high_uri_;  // related uri to high_tag
    std::size_t       high_ndx_;  // maximum index of the rule that is last valid
    std::size_t       max_;       // maximum index as whole
    std::vector<bool> available_; // list of available paths (true available; false not in the right subtree or already found)
                                  // NOLINTEND(misc-non-private-member-variables-in-classes)
  };

  class xpath_limits
  {
  public:
    explicit xpath_limits(const xpath_node_struct& xpaths)
    {
      data_.reserve(xpaths.max_xpath_size());
      for (auto cnt = 0UL; cnt < xpaths.max_xpath_size(); cnt++)
      {
        // p_limits tmp;
        auto low  = xpaths.first_xpath_tag_name(cnt);
        auto high = xpaths.last_xpath_tag_name(cnt);
        auto tmp  = p_limits( //
          low.first,
          "", // FIXME  ostri : load uri value
          low.second,
          high.first, // FIXME load uri value
          "",
          high.second,
          xpaths.size());
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
      int         cnt = 0;
      for (const auto& el : data_)
        msg += fmt::format("{}- {}: low:[{}:{}] high:[{}:{}]\n", //
                           std::string(offs, ' '),
                           cnt++,
                           el.low_tag(),
                           el.low_ndx(),
                           el.high_tag(),
                           el.high_ndx());
      return fmt::format("\n{}", msg);
    }
  private:
    std::vector<p_limits> data_;
  };
} // namespace fsp