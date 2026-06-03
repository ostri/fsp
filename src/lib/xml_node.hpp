#pragma once

#include <fmt/format.h>
#include <string>
namespace fsp
{
  using cstr_t = std::string_view;
  class xml_node
  {
  public:
    xml_node(cstr_t uri, cstr_t tag);
    xml_node()                           = default;
    ~xml_node()                          = default;
    xml_node(const xml_node&)            = default;
    xml_node(xml_node&&)                 = default;
    xml_node& operator=(const xml_node&) = default;
    xml_node& operator=(xml_node&&)      = default;
    xml_node(const char* uri, const char* tag);
    [[nodiscard]] std::string uri() const;
    [[nodiscard]] std::string tag() const;
    [[nodiscard]] std::string dump(int offs = 0);
  private:
    std::string tag_;
    std::string uri_;
  };
} // namespace fsp