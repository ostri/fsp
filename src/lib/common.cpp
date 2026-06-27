#include "common.hpp"
namespace fsp
{

  std::string escape_xml_attr(std::string_view s)
  {
    std::string res;
    res.reserve(s.size() + 10); // NOLINT(readability-magic-numbers)
    for (char c : s)
    {
      switch (c)
      {
      case '&': res += "&amp;"; break;
      case '"': res += "&quot;"; break;
      case '\'': res += "&apos;"; break;
      case '<': res += "&lt;"; break;
      case '>': res += "&gt;"; break;
      default: res += c; break;
      }
    }
    return res;
  }

  str_XMLCh_t escape_xml_attr_xmlch(cstr_XMLCh_t s)
  {
    str_XMLCh_t res;
    res.reserve(s.size() + 10); // NOLINT(readability-magic-numbers)
    for (XMLCh c : s)
    {
      switch (c)
      {
      case u'&': res += u"&amp;"; break;
      case u'"': res += u"&quot;"; break;
      case u'\'': res += u"&apos;"; break;
      case u'<': res += u"&lt;"; break;
      case u'>': res += u"&gt;"; break;
      default: res += c; break;
      }
    }
    return res;
  }
} // namespace fsp