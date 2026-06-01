#pragma once

#include <string>
#include <vector>

namespace fsp
{

  class static_sorted_set
  {
  public:
    // NOLINTNEXTLINE(readability-magic-numbers)
    explicit static_sorted_set(std::size_t capacity = 30);
    void                      insert(int value);
    void                      erase(int value);
    [[nodiscard]] std::string dump() const;
  private:
    std::vector<int> data_;
  };
} // namespace fsp