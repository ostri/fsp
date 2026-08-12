#include "xpath_helpers.hpp"
#include <fmt/format.h>

namespace fsp::xpath_helpers
{

  //   // Parses an XPath string into xpath_t (vector<e_tag>).
  //   // Supports:
  //   //   /Document/iso:FIToFICstmrCdtTrf/CdtTrfTxInf
  //   //   Document/child/grandchild
  //   //   /root
  //   result<xpath_t> from_string(const str_t& xpath_str)
  //   {
  //     if (xpath_str.empty()) { return std::unexpected(error_info{processor_error::invalid_xpath, "XPath is empty", "", 0}); }

  //     xpath_t          result;
  //     cstr_t path = xpath_str;

  //     // Strip the leading '/'
  //     if (path.starts_with('/')) path.remove_prefix(1);

  //     if (path.empty())
  //     {
  //       return std::unexpected(error_info{processor_error::invalid_xpath, fmt::format("XPath '{}' has no elements", xpath_str), "", 0});
  //     }

  //     while (! path.empty())
  //     {
  //       auto sep     = path.find('/');
  //       auto segment = (sep == cstr_t::npos) ? path : path.substr(0, sep);

  //       if (segment.empty())
  //       {
  //         return std::unexpected(
  //           error_info{processor_error::invalid_xpath, fmt::format("XPath '{}' contains an empty segment", xpath_str), "", 0});
  //       }

  //       e_tag tag;
  //       auto  colon = segment.find(':');
  //       if (colon != cstr_t::npos)
  //       {
  //         tag.set_ns(str_t(segment.substr(0, colon)));
  //         tag.set_tag(str_t(segment.substr(colon + 1)));
  //       }
  //       else
  //       {
  //         tag.set_tag(str_t(segment));
  //       }
  //       result.push_back(std::move(tag));

  //       path = (sep == cstr_t::npos) ? cstr_t{} : path.substr(sep + 1);
  //     }

  //     if (result.empty())
  //     {
  //       return std::unexpected(
  //         error_info{processor_error::invalid_xpath, fmt::format("XPath '{}': no valid elements", xpath_str), "", 0});
  //     }

  //     return result;
  //   }

  //   str_t to_string(const xpath_t& xpath)
  //   {
  //     str_t result;
  //     for (const auto& tag : xpath)
  //     {
  //       result += '/';
  //       result += tag.to_string();
  //     }
  //     return result;
  //   }

  //   bool validate(const xpath_t& xpath, str_t* error_msg) // TODO: ostri - bug 2: error_msg as result is not ok.
  //   {
  //     if (xpath.empty())
  //     {
  //       if (nullptr != error_msg) *error_msg = "XPath is empty";
  //       return false;
  //     }
  //     for (const auto& tag : xpath)
  //     {
  //       if (tag.tag().empty())
  //       {
  //         if (nullptr != error_msg) *error_msg = "XPath contains a tag with an empty name";
  //         return false;
  //       }
  //     }
  //     return true;
  //   }

  //   std::optional<e_tag> last_tag(const xpath_t& xpath)
  //   {
  //     if (xpath.empty()) return std::nullopt;
  //     return xpath.back();
  //   }

  //   std::size_t depth(const xpath_t& xpath) { return xpath.size(); }

} // namespace fsp::xpath_helpers
