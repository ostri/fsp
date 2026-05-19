#include "xpath_helpers.hpp"
#include "common.hpp"


namespace fsp::xpath_helpers
{
  result<e_tag> parse_e_tag(const std::string& etag_str)
  {
    e_tag  result;
    size_t colon_pos = etag_str.find(':');

    if (colon_pos == 0)
    {
      return std::unexpected(error_info{processor_error::invalid_xpath, fmt::format("Empty namespace in tag: '{}'", etag_str), "", 0});
    }

    if (colon_pos != std::string::npos)
    {
      result.set_ns(etag_str.substr(0, colon_pos));
      result.set_tag(etag_str.substr(colon_pos + 1));

      if (result.tag().empty())
      {
        return std::unexpected(
          error_info{processor_error::invalid_xpath, fmt::format("Empty tag name after namespace: '{}'", etag_str), "", 0});
      }
    }
    else
    {
      result.set_tag(etag_str);
    }

    if (result.tag().empty()) //
    {
      return std::unexpected(error_info{processor_error::invalid_xpath, "Empty tag name", "", 0});
    }

    return result;
  }

  result<xpath_t> from_string(const std::string& xpath_str)
  {
    xpath_t result;
    auto    path = static_cast<std::string>(fsp::trim(xpath_str));

    // Remove leading slash if present
    if (! path.empty() && path[0] == '/') path = path.substr(1);

    // Remove trailing slash if present
    if (! path.empty() && path.back() == '/') path.pop_back();

    if (path.empty())
    {
      return std::unexpected(
        error_info{processor_error::invalid_xpath, fmt::format("Empty XPath after normalization: '{}'", xpath_str), "", 0});
    }

    size_t start = 0;
    size_t end   = path.find('/');

    while (end != std::string::npos)
    {
      std::string etag_str = path.substr(start, end - start);
      if (! etag_str.empty())
      {
        auto tag_result = parse_e_tag(etag_str);
        if (! tag_result) return std::unexpected(tag_result.error());
        result.push_back(std::move(*tag_result));
      }
      start = end + 1;
      end   = path.find('/', start);
    }

    std::string last_etag = path.substr(start);
    if (! last_etag.empty())
    {
      auto tag_result = parse_e_tag(last_etag);
      if (! tag_result) return std::unexpected(tag_result.error());
      result.push_back(std::move(*tag_result));
    }

    if (result.empty())
    {
      return std::unexpected(
        error_info{processor_error::invalid_xpath, fmt::format("No valid tags found in XPath: '{}'", xpath_str), "", 0});
    }

    return result;
  }

  std::string to_string(const xpath_t& xpath)
  {
    if (xpath.empty()) return "";

    std::string result;
    for (const auto& et : xpath)
    {
      if (! result.empty()) result += "/";
      result += et.to_string();
    }
    return result;
  }

  bool validate(const xpath_t& xpath, std::string* error_msg)
  {
    if (xpath.empty())
    {
      if (error_msg->empty()) *error_msg = "XPath is empty";
      return false;
    }

    for (size_t i = 0; i < xpath.size(); ++i)
    {
      const auto& et = xpath[i];
      if (et.tag().empty())
      {
        if (nullptr != error_msg) *error_msg = fmt::format("Empty tag at position {}", i);
        return false;
      }

      if (et.tag().find_first_of(" \t\n\r<>") != std::string::npos)
      {
        if (nullptr != error_msg) *error_msg = fmt::format("Invalid characters in tag '{}' at position {}", et.tag(), i);
        return false;
      }
    }

    return true;
  }

  std::optional<e_tag> last_tag(const xpath_t& xpath)
  {
    if (xpath.empty()) return std::nullopt;
    return xpath.back();
  }

  size_t depth(const xpath_t& xpath) { return xpath.size(); }
} // namespace fsp::xpath_helpers