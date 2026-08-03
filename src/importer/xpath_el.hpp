#pragma once
#include <string_view>
namespace fsp
{
  using cstr_t = std::string_view;
  // Dummy class for context
  struct xpath_el
  {
  public:
    cstr_t ns;
    cstr_t tag;
  };
} // namespace fsp