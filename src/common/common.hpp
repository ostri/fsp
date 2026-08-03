#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <xercesc/util/PlatformUtils.hpp>
#include <filesystem>

namespace fsp
{
  using cstr_XMLCh_t = std::basic_string_view<XMLCh>;
  using str_XMLCh_t  = std::basic_string<XMLCh>;
  using cstr_t       = std::string_view;
  using str_t        = std::string;
  // Whitespace characters to trim
  constexpr cstr_t WHITESPACE = " \t\n\r";
  // Trims whitespace from the start of the string_view
  [[nodiscard]] constexpr cstr_t ltrim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;
  // Trims whitespace from the end of the string_view
  [[nodiscard]] constexpr cstr_t rtrim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;
  // Trims whitespace from both ends of the string_view
  [[nodiscard]] constexpr cstr_t trim(cstr_t str, cstr_t ws = WHITESPACE) noexcept;

  str_t escape_xml_attr(cstr_t s);
  void  escape_xml_attr_xmlch(cstr_XMLCh_t s, str_XMLCh_t& out);

  constexpr cstr_t ltrim(cstr_t str, cstr_t ws) noexcept
  {
    const auto start = str.find_first_not_of(ws);
    return (start == cstr_t::npos) ? cstr_t{} : str.substr(start);
  }
  constexpr cstr_t rtrim(cstr_t str, cstr_t ws) noexcept
  {
    const auto end = str.find_last_not_of(ws);
    return (end == cstr_t::npos) ? cstr_t{} : str.substr(0, end + 1);
  }
  constexpr cstr_t trim(cstr_t str, cstr_t ws) noexcept { return rtrim(ltrim(str, ws), ws); }

  consteval bool is_release()
  {
#ifdef NDEBUG
    return true;
#else
    return false;
#endif
  }
  consteval bool is_debug() { return ! is_release(); }

  namespace fs = std::filesystem;
  struct param
  {
    std::vector<str_t> files;    // files to be parsed
    str_t              xsd_file; // path to the grammar file (can be empty)
    str_t              p_name;   // program name
  };
  inline param load_args(const int argc, const char** argv)
  {
    struct param arg;
    arg.p_name = *argv;
    std::vector<cstr_t> raw_args(argv + 1, argv + argc); // NOLINT (cppcoreguidelines-pro-bounds-pointer-arithmetic)

    for (const auto& file : raw_args)
    {
      auto fn = fs::absolute(file).lexically_normal().string();
      if (fn.ends_with(".xsd")) arg.xsd_file = fn;
      else arg.files.emplace_back(fn);
    }
    return arg;
  }

} // namespace fsp