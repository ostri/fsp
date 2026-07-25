#include "xpath_helpers.hpp"
#include <fmt/format.h>

namespace fsp::xpath_helpers
{

//   // Razčleni XPath string v xpath_t (vector<e_tag>).
//   // Podpira:
//   //   /Document/iso:FIToFICstmrCdtTrf/CdtTrfTxInf
//   //   Document/child/grandchild
//   //   /root
//   result<xpath_t> from_string(const std::string& xpath_str)
//   {
//     if (xpath_str.empty()) { return std::unexpected(error_info{processor_error::invalid_xpath, "XPath je prazen", "", 0}); }

//     xpath_t          result;
//     std::string_view path = xpath_str;

//     // Odstranimo vodilni '/'
//     if (path.starts_with('/')) path.remove_prefix(1);

//     if (path.empty())
//     {
//       return std::unexpected(error_info{processor_error::invalid_xpath, fmt::format("XPath '{}' nima elementov", xpath_str), "", 0});
//     }

//     while (! path.empty())
//     {
//       auto sep     = path.find('/');
//       auto segment = (sep == std::string_view::npos) ? path : path.substr(0, sep);

//       if (segment.empty())
//       {
//         return std::unexpected(
//           error_info{processor_error::invalid_xpath, fmt::format("XPath '{}' vsebuje prazen segment", xpath_str), "", 0});
//       }

//       e_tag tag;
//       auto  colon = segment.find(':');
//       if (colon != std::string_view::npos)
//       {
//         tag.set_ns(std::string(segment.substr(0, colon)));
//         tag.set_tag(std::string(segment.substr(colon + 1)));
//       }
//       else
//       {
//         tag.set_tag(std::string(segment));
//       }
//       result.push_back(std::move(tag));

//       path = (sep == std::string_view::npos) ? std::string_view{} : path.substr(sep + 1);
//     }

//     if (result.empty())
//     {
//       return std::unexpected(
//         error_info{processor_error::invalid_xpath, fmt::format("XPath '{}': ni veljavnih elementov", xpath_str), "", 0});
//     }

//     return result;
//   }

//   std::string to_string(const xpath_t& xpath)
//   {
//     std::string result;
//     for (const auto& tag : xpath)
//     {
//       result += '/';
//       result += tag.to_string();
//     }
//     return result;
//   }

//   bool validate(const xpath_t& xpath, std::string* error_msg) // TODO: ostri - bug 2: error_msg as result is not ok.
//   {
//     if (xpath.empty())
//     {
//       if (nullptr != error_msg) *error_msg = "XPath je prazen";
//       return false;
//     }
//     for (const auto& tag : xpath)
//     {
//       if (tag.tag().empty())
//       {
//         if (nullptr != error_msg) *error_msg = "XPath vsebuje tag s praznim imenom";
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
