#pragma once

#include <fmt/format.h>
#include <string>
namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;
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
    [[nodiscard]] str_t uri() const;
    [[nodiscard]] str_t tag() const;
    [[nodiscard]] str_t dump(int offs = 0);
  private:
    str_t tag_;
    str_t uri_;
  };
} // namespace fsp