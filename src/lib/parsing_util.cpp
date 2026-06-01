#include "parsing_util.hpp"

namespace fsp
{

  [[nodiscard]] std::string xml_attr::full_xpath_with_uri() const
  {
    std::string tmp;
    for (std::size_t i = 0; i < xpath_size_; ++i) tmp += fmt::format("/{}:{}", xpath_[i].ns, xpath_[i].tag);

    if (is_attr()) tmp += fmt::format("/@{}:{}", attr_uri(), attr_name());
    else if (is_array()) tmp += "/*";

    return tmp;
  }


} // namespace fsp