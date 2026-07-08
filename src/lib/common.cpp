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

  void escape_xml_attr_xmlch(cstr_XMLCh_t s, str_XMLCh_t& out)
  {
    // str_XMLCh_t res;
    // res.reserve(s.size() + 10); // NOLINT(readability-magic-numbers)
    for (XMLCh c : s)
    {
      switch (c)
      {
      case u'&': out += u"&amp;"; break;
      case u'"': out += u"&quot;"; break;
      case u'\'': out += u"&apos;"; break;
      case u'<': out += u"&lt;"; break;
      case u'>': out += u"&gt;"; break;
      default: out += c; break;
      };
    }
    // return res;
  }
} // namespace fsp