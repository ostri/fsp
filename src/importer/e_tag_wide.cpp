#include "e_tag_wide.hpp"

namespace fsp
{
  e_tag_wide::e_tag_wide(cstr_t ns, cstr_t tag)
  : ns_(ns)
  , tag_(tag)
  {
  }
  e_tag_wide::e_tag_wide(cstr_XMLCh_t ns, cstr_XMLCh_t tag)
  : ns_(ns)
  , tag_(tag)
  {
  }
  [[nodiscard]] const x_str& e_tag_wide::ns() const { return ns_; }
  [[nodiscard]] const x_str& e_tag_wide::tag() const { return tag_; }
  void                       e_tag_wide::set_tag(const x_str& tag) { tag_.assign(tag.data()); };
  void                       e_tag_wide::set_tag(const XMLCh* tag) { tag_.assign(tag); }
  void                       e_tag_wide::set_ns(const x_str& ns) { ns_.assign(ns.data()); }
  void                       e_tag_wide::set_ns(const XMLCh* ns) { ns_.assign(ns); }
}; // namespace fsp