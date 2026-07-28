#include "xml_node.hpp"
namespace fsp
{

  xml_node::xml_node(const char* uri, const char* tag)
  : tag_(tag)
  , uri_(uri)
  {
  }
  xml_node::xml_node(cstr_t uri, cstr_t tag)
  : tag_(tag)
  , uri_(uri)
  {
  }
  [[nodiscard]] str_t xml_node::uri() const { return uri_; }
  [[nodiscard]] str_t xml_node::tag() const { return tag_; }
  [[nodiscard]] str_t xml_node::dump(int offs) { return fmt::format("{}[tag:{} uri:{}]", str_t(offs, ' '), tag_, uri_); }
} // namespace fsp