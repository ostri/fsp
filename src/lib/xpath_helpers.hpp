#pragma once


// Helper functions for working with e_tag and xpath_t
#include "e_tag.hpp"
#include "error_info.hpp"
#include <expected>
#include <optional>

namespace fsp
{
  // Type aliases for expected patterns
  template <typename T>
  using result      = std::expected<T, error_info>;
  using void_result = std::expected<void, error_info>;
} // namespace fsp

namespace fsp::xpath_helpers
{
//   // split xx:yy
//   // result<e_tag> parse_e_tag(const std::string& etag_str);

//   // Parse XPath string like "/ns:root/child/grandchild" or "root/child"
//   result<xpath_t> from_string(const std::string& xpath_str);

//   // Convert xpath_t to string representation
//   std::string to_string(const xpath_t& xpath);

//   // Validate xpath_t (no empty tags, valid characters, etc.)
//   bool validate(const xpath_t& xpath, std::string* error_msg = nullptr);

//   // Get the last tag from xpath
//   std::optional<e_tag> last_tag(const xpath_t& xpath);

//   // Get depth of xpath
//   size_t depth(const xpath_t& xpath);
} // namespace fsp::xpath_helpers
