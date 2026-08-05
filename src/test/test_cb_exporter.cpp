#include "cb_exporter.hpp"
#include <catch2/catch_test_macros.hpp>

namespace
{
  struct test_txn_t : fsp::transaction_t
  {
  };

  struct test_qual_t : fsp::qualificators_t
  {
  };

  // Minimal concrete cb_exporter: every method returns a trivially-successful value; state_
  // is the only thing this test cares about (clone() independence).
  class demo_cb : public fsp::cb_exporter_crtp<demo_cb, test_txn_t, test_qual_t>
  {
  public:
    int state_ = 0; // NOLINT(misc-non-private-member-variables-in-classes) -- test-only, no trailing underscore rule doesn't apply here
                    // (deliberately public for the test)

    [[nodiscard]] fsp::exp_result<fsp::str_t> fetch_doc_name(const test_qual_t& /*qualifiers*/,
                                                             fsp::cstr_t /*path*/,
                                                             int /*drain_id*/,
                                                             std::size_t /*block_number*/,
                                                             std::size_t /*total_blocks*/,
                                                             fsp::cstr_t /*filename_prefix*/) override
    { return fsp::str_t("doc.xml"); }

    [[nodiscard]] fsp::exp_result<run_stat_t> fetch_run_stat(const test_qual_t& /*qualifiers*/, int /*drain_id*/) override
    { return run_stat_t{}; }

    [[nodiscard]] fsp::fetch_doc_data_result_t<test_txn_t> fetch_doc_data(const test_qual_t& /*qualifiers*/,
                                                                          int /*drain_id*/,
                                                                          std::uint64_t /*doc_id*/) override
    { return fsp::fetch_doc_data_result_t<test_txn_t>{.status = fsp::fetch_doc_data_status::no_more_data, .block = {}, .error = {}}; }

    [[nodiscard]] fsp::exp_result<fsp::str_t> prepare_transaction(std::size_t /*ndx*/,
                                                                  int /*drain_id*/,
                                                                  std::uint64_t /*doc_id*/,
                                                                  const test_txn_t& /*data*/) override
    { return fsp::str_t(); }

    [[nodiscard]] fsp::exp_result<fsp::str_t> prepare_header(const test_qual_t& /*qualifiers*/,
                                                             int /*drain_id*/,
                                                             std::uint64_t /*doc_id*/,
                                                             const fsp::txn_block_t<test_txn_t>& /*block*/) override
    { return fsp::str_t(); }

    [[nodiscard]] fsp::exp_result<fsp::str_t> prepare_footer(const test_qual_t& /*qualifiers*/,
                                                             int /*drain_id*/,
                                                             std::uint64_t /*doc_id*/,
                                                             const fsp::txn_block_t<test_txn_t>& /*block*/) override
    { return fsp::str_t(); }

    [[nodiscard]] bool document_prepared(const test_qual_t& /*qualifiers*/, int /*drain_id*/, std::uint64_t /*doc_id*/) override
    { return true; }
  };
} // namespace

TEST_CASE("cb_exporter_crtp::clone() produces an independent instance of the derived type", "[cb_exporter][positive]")
{
  demo_cb original;
  original.state_ = 1;

  const std::unique_ptr<fsp::cb_exporter<test_txn_t, test_qual_t>> cloned = original.clone();
  REQUIRE(cloned != nullptr);

  // Mutate the clone through the base pointer isn't possible (state_ is on the derived type),
  // so instead verify the clone starts out as an independent COPY -- mutating the original
  // after cloning must not be visible through the clone.
  original.state_ = 2;
  // Safe downcast: clone() is known (by demo_cb's own clone() contract, via cb_exporter_crtp) to
  // always return a demo_cb instance -- see feedback_static_cast_downcast_nolint memory note.
  const auto* derived_clone = static_cast<const demo_cb*>(cloned.get()); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
  REQUIRE(derived_clone != nullptr);
  CHECK(derived_clone->state_ == 1);
  CHECK(original.state_ == 2);
}

TEST_CASE("cb_exporter_crtp::clone() returns a non-null cb_exporter<T,Q> base pointer", "[cb_exporter][negative]")
{
  const demo_cb original;
  const auto    cloned = original.clone();
  CHECK(cloned != nullptr);
}
