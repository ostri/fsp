#include "exporter_error.hpp"
#include <catch2/catch_test_macros.hpp>

using fsp::ev_result;
using fsp::exp_error;
using fsp::exp_error_info;
using fsp::exp_result;

// --- default construction ---------------------------------------------------------------------

TEST_CASE("exp_error_info default-constructs to 'unknown' with empty fields", "[exporter_error][positive]")
{
  const exp_error_info e;
  CHECK(e.code() == exp_error::unknown);
  CHECK(e.message().empty());
  CHECK(e.path().empty());
  CHECK(e.drain_id() == -1);
  CHECK(e.doc_id() == 0);
}

// --- accessors ---------------------------------------------------------------------------------

TEST_CASE("exp_error_info accessors round-trip the constructor's arguments", "[exporter_error][positive]")
{
  const exp_error_info e(exp_error::file_move_failed, "disk full", "/tmp/out.xml", 7, 42); // NOLINT(readability-magic-numbers)
  CHECK(e.code() == exp_error::file_move_failed);
  CHECK(e.message() == "disk full");
  CHECK(e.path() == "/tmp/out.xml");
  CHECK(e.drain_id() == 7);
  CHECK(e.doc_id() == 42);
}

TEST_CASE("exp_error_info defaults drain_id/doc_id when not supplied", "[exporter_error][negative]")
{
  const exp_error_info e(exp_error::invalid_config, "empty drain_list");
  CHECK(e.drain_id() == -1);
  CHECK(e.doc_id() == 0);
  CHECK(e.path().empty());
}

// --- to_string ------------------------------------------------------------------------------

TEST_CASE("exp_error_info::to_string includes the code name and message", "[exporter_error][positive]")
{
  const exp_error_info e(exp_error::document_rejected, "document_prepared() returned false");
  const auto           s = e.to_string();
  CHECK(s.contains("document_rejected"));
  CHECK(s.contains("document_prepared() returned false"));
}

TEST_CASE("exp_error_info::to_string includes drain/doc ids when set", "[exporter_error][positive]")
{
  const exp_error_info e(exp_error::fetch_doc_data_failed, "boom", "", 3, 9); // NOLINT(readability-magic-numbers)
  const auto           s = e.to_string();
  CHECK(s.contains("drain:3"));
  CHECK(s.contains("doc:9"));
}

TEST_CASE("exp_error_info::to_string omits drain/doc ids when unset", "[exporter_error][negative]")
{
  const exp_error_info e(exp_error::unknown, "boom");
  const auto           s = e.to_string();
  CHECK_FALSE(s.contains("drain:"));
  CHECK_FALSE(s.contains("doc:"));
}

TEST_CASE("exp_error_info::to_string includes the path when set", "[exporter_error][positive]")
{
  const exp_error_info e(exp_error::file_open_failed, "cannot open", "/tmp/a.xml");
  const auto           s = e.to_string();
  CHECK(s.contains("/tmp/a.xml"));
}

// --- exp_result / exp_void_result -------------------------------------------------------------

TEST_CASE("exp_result<T> carries a value on success and an exp_error_info on failure", "[exporter_error][positive]")
{
  const exp_result<int> ok = 5; // NOLINT(readability-magic-numbers)
  REQUIRE(ok.has_value());
  CHECK(*ok == 5); // NOLINT(readability-magic-numbers)

  const exp_result<int> err = std::unexpected(exp_error_info(exp_error::unknown, "nope"));
  REQUIRE_FALSE(err.has_value());
  CHECK(err.error().code() == exp_error::unknown);
}

TEST_CASE("exp_void_result default success carries no error", "[exporter_error][negative]")
{
  const ev_result ok;
  CHECK(ok.has_value());
}
