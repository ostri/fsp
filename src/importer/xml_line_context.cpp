#include "xml_line_context.hpp"
#include <fmt/format.h>

namespace fsp
{
  namespace
  {
    // Byte offset (into doc) of the first character of 1-based line row, or cstr_t::npos if row
    // is beyond the document's own last line - see context_around()'s own doc comment.
    [[nodiscard]] std::size_t line_start_of(cstr_t doc, XMLFileLoc row)
    {
      if (row < 1) return cstr_t::npos;
      std::size_t pos     = 0;
      XMLFileLoc  cur_row = 1;
      while (cur_row < row)
      {
        pos = doc.find('\n', pos);
        if (pos == cstr_t::npos) return cstr_t::npos;
        ++pos;
        ++cur_row;
      }
      return pos;
    }

    // line starting at start, up to (not including) the next '\n' or end of doc - trailing '\r'
    // (CRLF input) is trimmed so a caller joining several of these into one single-line log record
    // never embeds a stray control character.
    [[nodiscard]] cstr_t line_at(cstr_t doc, std::size_t start)
    {
      auto end = doc.find('\n', start);
      if (end == cstr_t::npos) end = doc.size();
      auto line = doc.substr(start, end - start);
      if (! line.empty() && line.back() == '\r') line.remove_suffix(1);
      return line;
    }
  } // namespace

  str_t context_around(cstr_t doc, XMLFileLoc row)
  {
    const auto start = line_start_of(doc, row);
    if (start == cstr_t::npos) return {};
    str_t out;
    if (row > 1)
    {
      const auto prev_start = line_start_of(doc, row - 1);
      if (prev_start != cstr_t::npos) out += fmt::format("{}: '{}' | ", row - 1, line_at(doc, prev_start));
    }
    out += fmt::format("{}: '{}'", row, line_at(doc, start));
    const auto next_start = line_start_of(doc, row + 1);
    if (next_start != cstr_t::npos) out += fmt::format(" | {}: '{}'", row + 1, line_at(doc, next_start));
    return out;
  }
} // namespace fsp
