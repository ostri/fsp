#pragma once

#include "common.hpp"
#include "x_str.hpp"

namespace fsp
{


  class e_tag_wide
  {
  public:
    e_tag_wide() = default;
    e_tag_wide(cstr_t ns, cstr_t tag);
    e_tag_wide(cstr_XMLCh_t ns, cstr_XMLCh_t tag);
    [[nodiscard]] const x_str& ns() const;
    [[nodiscard]] const x_str& tag() const;
    void                       set_tag(const x_str& tag);
    void                       set_tag(const XMLCh* tag);
    void                       set_ns(const x_str& ns);
    void                       set_ns(const XMLCh* ns);
  private:
    x_str ns_;  // namespace: prefix or uri
    x_str tag_; // tagname
  };
  using xpath_wide_t = std::vector<e_tag_wide>;
}; // namespace fsp