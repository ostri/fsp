#pragma once

/**
 * @file exporter_error.hpp
 * @brief Error vocabulary for the fsp::exporter module.
 *
 * Deliberately independent of fsp::processor_error / fsp::error_info (see
 * src/importer/error_info.hpp) -- those enumerate import-specific failures
 * (XSD validation, XML well-formedness, ...) that have no meaning here.
 * exporter_error_info follows the same shape (code + message + location,
 * magic_enum-based to_string()) purely because it is a proven, readable
 * pattern in this codebase, not because the two types are related.
 *
 * fsp::xml_writer (src/exporter/xml_writer.hpp) keeps using fsp::e_result /
 * fsp::error_info internally and is left untouched; callers that observe an
 * xml_writer failure translate it into an exporter_error_info at the call
 * site (see exporter_error::xml_writer_error below).
 */

#include <cstdint>
#include <expected>
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <string>
#include <string_view>

namespace fsp
{
  using cstr_t = std::string_view;
  using str_t  = std::string;

  /// @brief Failure categories an exp_result<T>/exp_void_result can carry.
  enum class exporter_error : std::uint8_t
  {
    unknown = 0,
    file_open_failed,           ///< xml_writer (or the collision probe) could not open the tmp file
    file_write_failed,          ///< xml_writer append/finalize failed
    file_move_failed,           ///< std::filesystem::rename() from tmp to its destination failed
    file_rename_collision,      ///< the filename-collision retry loop was exhausted
    invalid_config,             ///< e.g. empty drain_list, number_of_threads == 0
    fetch_doc_name_failed,      ///< cb_exporter::fetch_doc_name() returned an error
    fetch_run_stat_failed,      ///< cb_exporter::fetch_run_stat() returned an error
    fetch_doc_data_failed,      ///< cb_exporter::fetch_doc_data() reported fetch_doc_data_status::error
    prepare_transaction_failed, ///< cb_exporter::prepare_transaction() returned an error
    prepare_header_failed,      ///< cb_exporter::prepare_header() returned an error
    prepare_footer_failed,      ///< cb_exporter::prepare_footer() returned an error
    document_rejected,          ///< cb_exporter::document_prepared() returned false -- treated as fatal
    xml_writer_error,           ///< wraps a translated fsp::error_info message from xml_writer
    cancelled,                  ///< a stop was requested while this document was in flight
  };

  /**
   * @brief Carries an exporter_error code together with a human-readable message and,
   * where known, the document/drain the failure occurred on.
   *
   * Mirrors fsp::error_info's shape (src/importer/error_info.hpp) but is defined fresh here
   * rather than reused, since the two error vocabularies serve unrelated modules.
   */
  class exporter_error_info
  {
  public:
    exporter_error_info() = default;
    exporter_error_info(exporter_error code, str_t msg, cstr_t path = "", int drain_id = -1, std::uint64_t doc_id = 0);

    [[nodiscard]] str_t          to_string() const;
    [[nodiscard]] cstr_t         message() const;
    [[nodiscard]] exporter_error code() const;
    [[nodiscard]] cstr_t         path() const;
    [[nodiscard]] int            drain_id() const;
    [[nodiscard]] std::uint64_t  doc_id() const;
  private:
    exporter_error code_{exporter_error::unknown};
    str_t          message_;
    str_t          path_;
    int            drain_id_ = -1;
    std::uint64_t  doc_id_   = 0;
  };

  /// @brief Result type shared by cb_exporter's non-throwing methods and exporter's own internals.
  template <typename T>
  using exp_result = std::expected<T, exporter_error_info>;
  /// @brief Void specialization of exp_result, for methods that only signal success/failure.
  using exp_void_result = std::expected<void, exporter_error_info>;

  inline int           exporter_error_info::drain_id() const { return drain_id_; }
  inline std::uint64_t exporter_error_info::doc_id() const { return doc_id_; }
} // namespace fsp
