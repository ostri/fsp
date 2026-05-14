#pragma once

#include <string>
#include <vector>
#include <fmt/format.h>
// Element tag with namespace support
namespace fsp
{
  class e_tag
  {
  public:
    bool                      operator==(const e_tag& other) const;
    bool                      operator!=(const e_tag& other) const;
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string ns() const;
    [[nodiscard]] std::string tag() const;
    void                      set_ns(const std::string& ns);
    void                      set_tag(const std::string& tag);
  private:
    std::string ns_;  // namespace (can be empty)
    std::string tag_; // local name
  };
  // XPath is a sequence of e_tags (e.g., root/child/grandchild)
  using xpath_t = std::vector<e_tag>;
}; // namespace fsp