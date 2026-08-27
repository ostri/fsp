#include "exporter_error.hpp"

namespace fsp
{
  exp_error_info::exp_error_info(exp_error code, str_t msg, cstr_t path, drain_t drain_id, doc_id_t doc_id)
  : code_(code)
  , message_(std::move(msg))
  , path_(path)
  , drain_id_(drain_id)
  , doc_id_(doc_id)
  {
  }

  exp_error exp_error_info::code() const { return code_; }
  cstr_t    exp_error_info::message() const { return message_; }
  cstr_t    exp_error_info::path() const { return path_; }

  str_t exp_error_info::to_string() const
  {
    auto code_name = magic_enum::enum_name(code_);
    auto prefix    = fmt::format("[{}]", code_name);

    if (drain_id_ >= 0) { prefix = fmt::format("{} drain:{}", prefix, drain_id_); }
    if (doc_id_ > 0) { prefix = fmt::format("{} doc:{}", prefix, doc_id_); }

    if (! path_.empty()) { return fmt::format("{} {} (file: {})", prefix, message_, path_); }
    return fmt::format("{} {}", prefix, message_);
  }
} // namespace fsp
