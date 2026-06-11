#pragma once

#include <string>
#include <string_view>

namespace fsp
{

  using cstr_t = std::string_view;
  // Whitespace characters to trim
  constexpr cstr_t WHITESPACE = " \t\n\r";

  // Trims whitespace from the start of the string_view
  [[nodiscard]] constexpr cstr_t ltrim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;

  // Trims whitespace from the end of the string_view
  [[nodiscard]] constexpr cstr_t rtrim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;

  // Trims whitespace from both ends of the string_view
  [[nodiscard]] constexpr cstr_t trim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;

  std::string escape_xml_attr(std::string_view s);

  constexpr cstr_t ltrim(cstr_t str, cstr_t ws) noexcept
  {
    const auto start = str.find_first_not_of(ws);
    return (start == std::string_view::npos) ? std::string_view{} : str.substr(start);
  }
  constexpr cstr_t rtrim(cstr_t str, cstr_t ws) noexcept
  {
    const auto end = str.find_last_not_of(ws);
    return (end == std::string_view::npos) ? std::string_view{} : str.substr(0, end + 1);
  }
  constexpr cstr_t trim(cstr_t str, cstr_t ws) noexcept { return rtrim(ltrim(str, ws), ws); }


} // namespace fsp