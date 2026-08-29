#pragma once

/**
 * @file exporter_error.hpp
 * @brief Error vocabulary for the fsp::exporter module.
 *
 * Deliberately independent of fsp::processor_error / fsp::error_info (see
 * src/importer/error_info.hpp) -- those enumerate import-specific failures
 * (XSD validation, XML well-formedness, ...) that have no meaning here.
 * exp_error_info follows the same shape (code + message + location,
 * magic_enum-based to_string()) purely because it is a proven, readable
 * pattern in this codebase, not because the two types are related.
 *
 * fsp::xml_writer (src/exporter/xml_writer.hpp) keeps using fsp::e_result /
 * fsp::error_info internally and is left untouched; callers that observe an
 * xml_writer failure translate it into an exp_error_info at the call
 * site (see exp_error::xml_writer_error below).
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

  /// @brief Drain (recipient) id -- defined here (the module's most upstream header, no
  /// dependency on exporter_types.hpp) rather than in exporter_types.hpp itself, so every module
  /// that handles a drain_id (exporter_state, exporter_worker, exp_error_info, drain_dscr_t,
  /// cb_exporter, ...) shares the SAME type without a circular include (exporter_types.hpp
  /// already includes this file).
  using drain_t = int;
  /// @brief Document id. 64-bit: in the two-phase model (see cb_exporter::compute_drain_stat())
  /// this is a concrete callback's own snowflake id (e.g. ach's rtl::unique_id(), uint64_t), not a
  /// small sequential counter - see exporter_state::next_doc_id()'s own doc comment for the
  /// single-phase model's own (still supported) sequential-counter allocation, which fits in the
  /// same 64-bit type without truncation either.
  using doc_id_t = std::uint64_t;

  /// @brief Failure categories an exp_result<T>/exp_void_result can carry.
  enum class exp_error : std::uint8_t
  {
    unknown = 0,
    file_open_failed,           ///< xml_writer (or the collision probe) could not open the tmp file
    file_write_failed,          ///< xml_writer append/finalize failed
    file_move_failed,           ///< std::filesystem::rename() from tmp to its destination failed
    file_rename_collision,      ///< the filename-collision retry loop was exhausted
    invalid_config,             ///< e.g. empty drain_list, number_of_threads == 0
    wrk_start_failed,           ///< cb_exporter::on_wrk_start() returned an error
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
  class exp_error_info
  {
  public:
    exp_error_info() = default;
    exp_error_info(exp_error code, str_t msg, cstr_t path = "", drain_t drain_id = -1, doc_id_t doc_id = 0);

    [[nodiscard]] str_t     to_string() const;
    [[nodiscard]] cstr_t    message() const;
    [[nodiscard]] exp_error code() const;
    [[nodiscard]] cstr_t    path() const;
    [[nodiscard]] drain_t   drain_id() const;
    [[nodiscard]] doc_id_t  doc_id() const;
  private:
    exp_error code_{exp_error::unknown}; ///< exporter error code
    str_t     message_;                  ///< message associated with the error code
    str_t     path_;                     ///< path - normally denotes output file that exporter
                                         ///< produces
    drain_t  drain_id_ = -1;             ///< drain for which the file is/was prodiced
    doc_id_t doc_id_   = 0;              ///< id of the document that the error refers to
  };

  /// @brief Result type shared by cb_exporter's non-throwing methods and exporter's own internals.
  template <typename T>
  using exp_result = std::expected<T, exp_error_info>;
  /// @brief Void specialization of exp_result, for methods that only signal success/failure.
  using ev_result = std::expected<void, exp_error_info>;

  inline drain_t  exp_error_info::drain_id() const { return drain_id_; }
  inline doc_id_t exp_error_info::doc_id() const { return doc_id_; }
} // namespace fsp
