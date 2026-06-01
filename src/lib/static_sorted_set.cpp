#include "static_sorted_set.hpp"
#include <fmt/format.h>
#include <algorithm>

namespace fsp
{

  static_sorted_set::static_sorted_set(std::size_t capacity) { data_.reserve(capacity); }

  void static_sorted_set::insert(int value)
  {
    auto it = std::ranges::lower_bound(data_, value);
    if (it == data_.end() || *it != value) { data_.insert(it, value); }
  }

  void static_sorted_set::erase(int value)
  {
    auto it = std::ranges::lower_bound(data_, value);
    if (it != data_.end() && *it == value)
    {
      data_.erase(it); // Shifts elements, extremely fast for small N
    }
  }

  [[nodiscard]] std::string static_sorted_set::dump() const
  {
    std::string res;
    for (auto el : data_) res += fmt::format("{} ", el);
    return fmt::format("({})", res.substr(0, res.size() - 1));
  }

} // namespace fsp