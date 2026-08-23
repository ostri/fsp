#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <xercesc/util/XercesDefs.hpp>

namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;

  /**
   * @brief Recovers a few lines of source text around a (1-based) xerces line number, purely for
   * a human reading a log message - no parsing decision anywhere depends on this.
   * @details xercesc::SAXParseException (and the validation errors reported through
   * xercesc::ErrorHandler more generally) only ever gives a row/col, never the offending text
   * itself. This returns the line BEFORE row, row's own line, and the line AFTER, each prefixed
   * with its own 1-based line number (e.g. "5: '<foo>' | 6: '<bar/>' | 7: '</foo>'") so the reader
   * can tell which is which without re-counting - used by both Handler::prepare_msg() (C, the
   * cutter's own SAX error path) and validation_error_handler::record() (V, doc_validator.cpp's
   * own separate schema-validation path) against their own copy of the whole document (already
   * mapped in memory - see doc_cutter::cut()'s/doc_validator::validate()'s own set_doc()/
   * ds_dscr_[doc_ndx].string_view() call).
   * @param doc the whole document this row is a line number into.
   * @param row 1-based line number (xercesc::SAXParseException::getLineNumber()).
   * @return empty string if row is out of range (e.g. a bogus/0 line number some xerces error
   * paths report, or xerces reporting one line past the actual EOF for an unterminated document -
   * observed directly) rather than guessing - this is diagnostic best-effort, never allowed to
   * turn a parse error into a crash.
   */
  [[nodiscard]] str_t context_around(cstr_t doc, XMLFileLoc row);
} // namespace fsp
