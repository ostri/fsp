#pragma once

#include <string>
namespace fsp
{
  using cstr_t = std::string_view;
  class xml_node
  {
  public:
    xml_node() = default;
    xml_node(const char* uri, const char* tag)
    : uri_(uri)
    , tag_(tag)
    {
    }
    xml_node(cstr_t uri, cstr_t tag)
    : uri_(uri)
    , tag_(tag)
    {
    }
    [[nodiscard]] std::string uri() const { return uri_; }
    [[nodiscard]] std::string tag() const { return tag_; }
  private:
    std::string uri_;
    std::string tag_;
  };
} // namespace fsp