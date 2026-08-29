#include "exporter.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

using fsp::cb_exporter_crtp;
using fsp::exp_error;
using fsp::exp_result;
using fsp::exporter;
using fsp::exporter_config_t;
using fsp::exporter_drain_cfg_t;
using fsp::fetch_doc_data_result_t;
using fsp::fetch_doc_data_status;
using fsp::qualificators_t;
using fsp::transaction_t;
using fsp::txn_block_t;
using logger::Logger;
using logger::logger_config;

namespace
{
  namespace fs = std::filesystem;

  // --- shared test fixtures, mirroring test_xml_writer.cpp's style -------------------------

  class temp_dir_guard
  {
  public:
    temp_dir_guard()
    : dir_(fs::temp_directory_path() / ("fsp_exporter_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    {
      fs::create_directories(dir_ / "tmp");
      fs::create_directories(dir_ / "target");
      fs::create_directories(dir_ / "error");
    }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)                 = delete;
    temp_dir_guard& operator=(const temp_dir_guard&)      = delete;
    temp_dir_guard(temp_dir_guard&&)                      = delete;
    temp_dir_guard&           operator=(temp_dir_guard&&) = delete;
    [[nodiscard]] std::string tmp() const { return (dir_ / "tmp").string(); }
    [[nodiscard]] std::string target() const { return (dir_ / "target").string(); }
    [[nodiscard]] std::string error() const { return (dir_ / "error").string(); }
  private:
    fs::path                    dir_;
    static inline std::uint32_t counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  logger_config silent_log_cfg() { return logger_config{.console_level = logger::level::off, .file_level = logger::level::off}; }

  std::unique_ptr<Logger> make_silent_logger()
  {
    auto log_ptr = Logger::create(silent_log_cfg());
    REQUIRE(log_ptr.has_value());
    return std::move(*log_ptr);
  }

  std::string read_file(const std::string& path)
  {
    std::ifstream     in(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
  }

  std::size_t file_count(const std::string& dir)
  {
    if (! fs::exists(dir)) { return 0; }
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator{}));
  }

  // --- test transaction/qualifier types ------------------------------------------------------

  struct test_txn_t : transaction_t
  {
  };

  struct test_qual_t : qualificators_t
  {
  };

  exporter_config_t make_cfg(const temp_dir_guard& dirs, std::vector<exporter_drain_cfg_t> drains, std::size_t threads)
  {
    return exporter_config_t{
      .drain_list        = std::move(drains),
      .number_of_threads = threads,
      .filename_prefix   = "test",
      .tmp_dir           = dirs.tmp(),
      .target_dir        = dirs.target(),
      .error_dir         = dirs.error(),
    };
  }

  // --- configurable in-memory demo callback --------------------------------------------------

  /**
   * @brief A cb_exporter backed by in-memory per-drain transaction queues, configurable to fail
   * at any one specific step (for the error-propagation test cases). All shared state (the
   * per-drain transaction source, existing-doc-count map, and per-step failure flags) lives in a
   * separate "shared" struct behind a mutex, since every worker thread's clone points at the same
   * shared instance -- clone() copies the demo_cb wrapper (a shared_ptr), not the underlying data.
   */
  struct shared_fixture
  {
    std::mutex mtx;
    std::map<fsp::drain_t, std::vector<test_txn_t>>
                                        pending;       // drain_id -> remaining transactions, split into blocks by compute_drain_stat()
    std::map<fsp::drain_t, std::size_t> max_doc_txn;   // drain_id -> block size
    std::map<fsp::drain_t, std::size_t> existing_docs; // drain_id -> starting doc_id, mirrors the single-phase model's own
                                                       // fetch_run_stat()-based resume (see compute_drain_stat())
    std::map<fsp::doc_id_t, std::vector<test_txn_t>>
      blocks_by_doc_id; // filled by compute_drain_stat(), read by fetch_doc_data() - see that method's own doc comment on why the two-phase
                        // model needs this indirection
    bool        fail_fetch_doc_name      = false;
    bool        fail_compute_drain_stat  = false;
    bool        fail_fetch_doc_data      = false;
    bool        fail_prepare_transaction = false;
    bool        fail_prepare_header      = false;
    bool        fail_prepare_footer      = false;
    bool        always_reject            = false; // document_prepared() always returns false
    bool        always_taken_name        = false; // fetch_doc_name() always returns a name that already exists on disk
    std::string tmp_dir;
  };

  class demo_cb : public cb_exporter_crtp<demo_cb, test_txn_t, test_qual_t>
  {
  public:
    demo_cb(std::shared_ptr<shared_fixture> shared, const Logger& log)
    : cb_exporter_crtp<demo_cb, test_txn_t, test_qual_t>(log)
    , shared_(std::move(shared))
    {
    }

    [[nodiscard]] exp_result<fsp::str_t> fetch_doc_name(const test_qual_t& /*qualifiers*/,
                                                        fsp::cstr_t /*path*/,
                                                        fsp::drain_t drain_id,
                                                        blk_id_t     block_number,
                                                        blk_id_t /*total_blocks*/,
                                                        fsp::cstr_t /*filename_prefix*/,
                                                        fsp::cstr_t /*filename_ext*/) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_fetch_doc_name)
      {
        return std::unexpected(fsp::exp_error_info(exp_error::fetch_doc_name_failed, "fetch_doc_name failed"));
      }
      if (shared_->always_taken_name) { return fsp::str_t("collide.xml"); }
      // drain_id is embedded in the name -- block_number alone is only unique WITHIN one drain
      // (every drain's numbering starts at 1), so two drains would otherwise collide on the same
      // file name (see cb_exporter::fetch_doc_name()'s own doc comment for why drain_id is a
      // parameter here at all, despite doc/opis_exporterja.txt's original list omitting it).
      return fmt::format("doc_{}_{}.xml", drain_id, block_number);
    }

    /// @brief Not called by exporter_worker in the two-phase model (see cb_exporter.hpp's own
    /// fetch_run_stat() doc comment) - kept only because it is still a pure-virtual API member;
    /// returns plain, harmless values so nothing accidentally depends on it being reachable.
    [[nodiscard]] exp_result<run_stat_t> fetch_run_stat(const test_qual_t& /*qualifiers*/, fsp::drain_t drain_id) override
    {
      const std::scoped_lock lock(shared_->mtx);
      return run_stat_t{.remaining_txn_count = shared_->pending[drain_id].size(), .existing_doc_count = 0};
    }

    /**
     * @brief Phase 1: splits drain_id's own pending queue into blocks of at most max_doc_txn[drain_id]
     * transactions each, stores each block under a freshly allocated doc_id in blocks_by_doc_id
     * (fetch_doc_data() below reads it back from there), starting doc_id numbering at
     * existing_docs[drain_id]+1 -- mirrors the single-phase model's own fetch_run_stat()-based resume
     * behavior (see the "exporter honors ... existing_doc_count ..." TEST_CASE), now driven entirely
     * by this method instead.
     */
    [[nodiscard]] exp_result<std::vector<fsp::doc_id_t>> compute_drain_stat(const test_qual_t& /*qualifiers*/,
                                                                            fsp::drain_t drain_id) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_compute_drain_stat)
      {
        return std::unexpected(fsp::exp_error_info(exp_error::fetch_run_stat_failed, "compute_drain_stat failed"));
      }

      auto&             queue       = shared_->pending[drain_id];
      const std::size_t block_size  = shared_->max_doc_txn.contains(drain_id) ? shared_->max_doc_txn.at(drain_id) : queue.size();
      std::size_t       next_doc_id = shared_->existing_docs.contains(drain_id) ? shared_->existing_docs.at(drain_id) : 0;

      std::vector<fsp::doc_id_t> doc_ids;
      std::size_t                offset = 0;
      while (offset < queue.size())
      {
        const std::size_t take = std::min(block_size, queue.size() - offset);
        ++next_doc_id;
        const auto doc_id = static_cast<fsp::doc_id_t>(next_doc_id);
        shared_->blocks_by_doc_id[doc_id].assign(queue.begin() + static_cast<std::ptrdiff_t>(offset),
                                                 queue.begin() + static_cast<std::ptrdiff_t>(offset + take));
        doc_ids.push_back(doc_id);
        offset += take;
      }
      queue.clear(); // fully consumed into blocks_by_doc_id above
      return doc_ids;
    }

    /// @brief Phase 2: looks doc_id's own block back up from blocks_by_doc_id (see
    /// compute_drain_stat()'s own doc comment) - no queue/cursor logic here anymore.
    [[nodiscard]] fetch_doc_data_result_t<test_txn_t> fetch_doc_data(const test_qual_t& /*qualifiers*/,
                                                                     fsp::drain_t /*drain_id*/,
                                                                     fsp::doc_id_t doc_id) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_fetch_doc_data)
      {
        return fetch_doc_data_result_t<test_txn_t>{.status = fetch_doc_data_status::error,
                                                   .block  = {},
                                                   .error = fsp::exp_error_info(exp_error::fetch_doc_data_failed, "fetch_doc_data failed")};
      }
      const auto it = shared_->blocks_by_doc_id.find(doc_id);
      if (it == shared_->blocks_by_doc_id.end())
      {
        return fetch_doc_data_result_t<test_txn_t>{.status = fetch_doc_data_status::no_more_data, .block = {}, .error = {}};
      }
      return fetch_doc_data_result_t<test_txn_t>{.status = fetch_doc_data_status::ok, .block = it->second, .error = {}};
    }

    [[nodiscard]] exp_result<fsp::str_t> prepare_transaction(std::size_t /*ndx*/,
                                                             fsp::drain_t /*drain_id*/,
                                                             fsp::doc_id_t /*doc_id*/,
                                                             const test_txn_t& data) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_prepare_transaction)
      {
        return std::unexpected(fsp::exp_error_info(exp_error::prepare_transaction_failed, "prepare_transaction failed"));
      }
      return fmt::format("<txn id=\"{}\">{}</txn>", fsp::to_string(data.id), data.value);
    }

    [[nodiscard]] exp_result<fsp::str_t> prepare_header(const test_qual_t& /*qualifiers*/,
                                                        fsp::drain_t  drain_id,
                                                        fsp::doc_id_t doc_id,
                                                        const txn_block_t<test_txn_t>& /*block*/) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_prepare_header)
      {
        return std::unexpected(fsp::exp_error_info(exp_error::prepare_header_failed, "prepare_header failed"));
      }
      return fmt::format(R"(<?xml version="1.0" encoding="UTF-8"?><doc drain="{}" id="{}">)", drain_id, doc_id);
    }

    [[nodiscard]] exp_result<fsp::str_t> prepare_footer(const test_qual_t& /*qualifiers*/,
                                                        fsp::drain_t /*drain_id*/,
                                                        fsp::doc_id_t /*doc_id*/,
                                                        const txn_block_t<test_txn_t>& /*block*/) override
    {
      const std::scoped_lock lock(shared_->mtx);
      if (shared_->fail_prepare_footer)
      {
        return std::unexpected(fsp::exp_error_info(exp_error::prepare_footer_failed, "prepare_footer failed"));
      }
      return fsp::str_t("</doc>");
    }

    [[nodiscard]] bool document_prepared(const test_qual_t& /*qualifiers*/, fsp::drain_t /*drain_id*/, fsp::doc_id_t /*doc_id*/) override
    {
      const std::scoped_lock lock(shared_->mtx);
      return ! shared_->always_reject;
    }
  private:
    std::shared_ptr<shared_fixture> shared_;
  };

  test_txn_t make_txn(fsp::txn_id_t id, const fsp::str_t& value)
  {
    test_txn_t t;
    t.id    = id;
    t.type  = 1;
    t.value = value;
    return t;
  }
} // namespace

// --- end-to-end happy path -------------------------------------------------------------------

TEST_CASE("exporter produces documents for a single drain with a single worker thread", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "a"), make_txn(2, "b"), make_txn(3, "c")}; // NOLINT(readability-magic-numbers)
  shared->max_doc_txn[1] = 2; // NOLINT(readability-magic-numbers) -- two documents: [a,b] then [c]

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 2}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT

  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_documents == 2);      // NOLINT(readability-magic-numbers)
  CHECK(res->total_transactions == 3);   // NOLINT(readability-magic-numbers)
  CHECK(file_count(dirs.target()) == 2); // NOLINT(readability-magic-numbers)
  CHECK(file_count(dirs.tmp()) == 0);
}

TEST_CASE("exporter's produced documents contain the expected header/transaction/footer content", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "hello")};
  shared->max_doc_txn[1] = 10; // NOLINT(readability-magic-numbers)

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb cb(shared, *log_ptr);
  REQUIRE(exp.exec(cb).has_value());

  REQUIRE(file_count(dirs.target()) == 1);
  const auto content = read_file((fs::path(dirs.target()) / *fs::directory_iterator(dirs.target())).string());
  CHECK(content.contains(R"(<?xml version="1.0" encoding="UTF-8"?>)"));
  CHECK(content.contains("<txn id=\"1\">hello</txn>"));
  CHECK(content.contains("</doc>"));
}

TEST_CASE("exporter with multiple worker threads and multiple drains produces every expected document", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared = std::make_shared<shared_fixture>();
  for (int drain_id = 1; drain_id <= 2; ++drain_id) // NOLINT(readability-magic-numbers)
  {
    shared->max_doc_txn[drain_id] = 5;     // NOLINT(readability-magic-numbers)
    for (fsp::txn_id_t i = 0; i < 23; ++i) // NOLINT(readability-magic-numbers)
    {
      shared->pending[drain_id].push_back(make_txn(i + 1, "x"));
    }
  }

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs,
             {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 5},  // NOLINT(readability-magic-numbers)
              exporter_drain_cfg_t{.id = 2, .name = "BANKA2", .max_doc_txn = 5}}, // NOLINT(readability-magic-numbers)
             4),
    test_qual_t{},
    *log_ptr,
    "test");
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_transactions == 46);   // NOLINT(readability-magic-numbers) -- 23 * 2 drains
  CHECK(res->total_documents == 10);      // NOLINT(readability-magic-numbers) -- ceil(23/5) * 2 = 5*2
  CHECK(file_count(dirs.target()) == 10); // NOLINT(readability-magic-numbers)
}

// --- drain exhaustion --------------------------------------------------------------------------

TEST_CASE("exporter removes a drain from available work once its final partial block is consumed", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "a"), make_txn(2, "b")};
  shared->max_doc_txn[1] = 10; // NOLINT(readability-magic-numbers) -- block bigger than available txns -> one partial document

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 2), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_documents == 1);
  CHECK(res->total_transactions == 2);
}

// --- compute_drain_stat()'s own doc_id numbering ------------------------------------------------

TEST_CASE("exporter honors compute_drain_stat's starting doc_id offset", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "a")};
  shared->max_doc_txn[1] = 10; // NOLINT(readability-magic-numbers)
  shared->existing_docs[1] =
    5; // NOLINT(readability-magic-numbers) -- demo_cb's own compute_drain_stat() starts doc_id numbering after this

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb cb(shared, *log_ptr);
  REQUIRE(exp.exec(cb).has_value());

  // The new document's content embeds its doc_id in the header -- verify it starts after 5, not at 1.
  REQUIRE(file_count(dirs.target()) == 1);
  const auto content = read_file((fs::path(dirs.target()) / *fs::directory_iterator(dirs.target())).string());
  CHECK(content.contains(R"(id="6")"));
}

// --- document_prepared() == false is fatal ------------------------------------------------------

TEST_CASE("exporter treats document_prepared() == false as a fatal run error", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "a")};
  shared->max_doc_txn[1] = 10; // NOLINT(readability-magic-numbers)
  shared->always_reject  = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 2), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::document_rejected);
}

// --- filename collision retry -------------------------------------------------------------------

TEST_CASE("exporter retries with a suffixed name when the naive candidate already exists", "[exporter][positive]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  // Pre-create the name fetch_doc_name() would naturally return for block_number == 1.
  {
    std::ofstream pre_existing(fs::path(dirs.tmp()) / "doc_1.xml");
    pre_existing << "pre-existing";
  }

  auto shared            = std::make_shared<shared_fixture>();
  shared->pending[1]     = {make_txn(1, "a")};
  shared->max_doc_txn[1] = 10; // NOLINT(readability-magic-numbers)

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_documents == 1);
  CHECK(file_count(dirs.target()) == 1); // succeeded despite the pre-existing tmp/doc_1.xml
}

TEST_CASE("exporter surfaces a fatal error when every collision-retry candidate is taken", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared               = std::make_shared<shared_fixture>();
  shared->pending[1]        = {make_txn(1, "a")};
  shared->max_doc_txn[1]    = 10; // NOLINT(readability-magic-numbers)
  shared->always_taken_name = true;
  // resolve_unique_doc_name() retries "collide.xml" as "collide_1.xml", "collide_2.xml", ...,
  // "collide_10.xml" (MAX_ATTEMPTS == 10, a sequential -- not random -- suffix, see
  // exporter_worker.hpp) -- pre-create every one of those candidates so the retry loop is
  // guaranteed to exhaust all of its attempts.
  {
    std::ofstream(fs::path(dirs.tmp()) / "collide.xml") << "pre-existing";
    for (int i = 1; i <= 10; ++i) { std::ofstream(fs::path(dirs.tmp()) / fmt::format("collide_{}.xml", i)) << "pre-existing"; } // NOLINT
  }

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::file_rename_collision);
}

// --- error propagation from each cb_exporter method individually ---------------------------------

TEST_CASE("exporter surfaces a fatal error when fetch_doc_name fails", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();
  auto                 shared  = std::make_shared<shared_fixture>();
  shared->pending[1]           = {make_txn(1, "a")};
  shared->max_doc_txn[1]       = 10; // NOLINT(readability-magic-numbers)
  shared->fail_fetch_doc_name  = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::fetch_doc_name_failed);
}

TEST_CASE("exporter surfaces a fatal error when compute_drain_stat fails", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr    = make_silent_logger();
  auto                 shared     = std::make_shared<shared_fixture>();
  shared->pending[1]              = {make_txn(1, "a")};
  shared->max_doc_txn[1]          = 10; // NOLINT(readability-magic-numbers)
  shared->fail_compute_drain_stat = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::fetch_run_stat_failed);
}

TEST_CASE("exporter surfaces a fatal error when fetch_doc_data reports an error status", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();
  auto                 shared  = std::make_shared<shared_fixture>();
  shared->pending[1]           = {make_txn(1, "a")};
  shared->max_doc_txn[1]       = 10; // NOLINT(readability-magic-numbers)
  shared->fail_fetch_doc_data  = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::fetch_doc_data_failed);
}

TEST_CASE("exporter surfaces a fatal error when prepare_transaction fails", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr     = make_silent_logger();
  auto                 shared      = std::make_shared<shared_fixture>();
  shared->pending[1]               = {make_txn(1, "a")};
  shared->max_doc_txn[1]           = 10; // NOLINT(readability-magic-numbers)
  shared->fail_prepare_transaction = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::prepare_transaction_failed);
}

TEST_CASE("exporter surfaces a fatal error when prepare_header fails", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();
  auto                 shared  = std::make_shared<shared_fixture>();
  shared->pending[1]           = {make_txn(1, "a")};
  shared->max_doc_txn[1]       = 10; // NOLINT(readability-magic-numbers)
  shared->fail_prepare_header  = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::prepare_header_failed);
}

TEST_CASE("exporter surfaces a fatal error when prepare_footer fails", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();
  auto                 shared  = std::make_shared<shared_fixture>();
  shared->pending[1]           = {make_txn(1, "a")};
  shared->max_doc_txn[1]       = 10; // NOLINT(readability-magic-numbers)
  shared->fail_prepare_footer  = true;

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 1), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::prepare_footer_failed);
}

// --- config validation ---------------------------------------------------------------------------

TEST_CASE("exporter rejects an empty drain_list before starting any worker", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  exporter<test_txn_t, test_qual_t> exp(make_cfg(dirs, {}, 1), test_qual_t{}, *log_ptr, "test");
  auto                              shared = std::make_shared<shared_fixture>();
  demo_cb                           cb(shared, *log_ptr);
  const auto                        res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::invalid_config);
}

TEST_CASE("exporter rejects zero worker threads before starting any worker", "[exporter][negative]")
{
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 10}}, 0), test_qual_t{}, *log_ptr, "test"); // NOLINT
  auto       shared = std::make_shared<shared_fixture>();
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::invalid_config);
}

// --- two-phase model: thread/drain count edge cases -----------------------------------------

TEST_CASE("exporter with more worker threads than drains still produces every document", "[exporter][positive]")
{
  // 5 threads, 1 drain: n_phase1 = min(1,5) = 1 -- 4 threads skip phase 1 entirely and go
  // straight to phase 2 (see exporter<T,Q>::exec()'s own doc comment) - this is the "surplus
  // threads" case the user asked to be measured/verified, not just the "fewer threads than
  // drains" case every OTHER multi-thread test case here already exercises incidentally.
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto shared            = std::make_shared<shared_fixture>();
  shared->max_doc_txn[1] = 3;                                                                    // NOLINT(readability-magic-numbers)
  for (fsp::txn_id_t i = 0; i < 37; ++i) { shared->pending[1].push_back(make_txn(i + 1, "x")); } // NOLINT(readability-magic-numbers)

  exporter<test_txn_t, test_qual_t> exp(
    make_cfg(dirs, {exporter_drain_cfg_t{.id = 1, .name = "BANKA1", .max_doc_txn = 3}}, 5), test_qual_t{}, *log_ptr, "test"); // NOLINT
  demo_cb    cb(shared, *log_ptr);
  const auto res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_transactions == 37);   // NOLINT(readability-magic-numbers)
  CHECK(res->total_documents == 13);      // NOLINT(readability-magic-numbers) -- ceil(37/3)
  CHECK(file_count(dirs.target()) == 13); // NOLINT(readability-magic-numbers)
}

TEST_CASE("exporter with fewer worker threads than drains work-steals across phase 1", "[exporter][positive]")
{
  // 2 threads, 5 drains: n_phase1 = min(5,2) = 2 -- both threads take part in phase 1, each
  // claiming/computing more than one drain's own block plan in turn (work-stealing via
  // pick_or_keep_drain(), see run_phase1()) before any thread reaches phase 2.
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto                              shared = std::make_shared<shared_fixture>();
  std::vector<exporter_drain_cfg_t> drains;
  for (std::uint8_t drain_id = 1; drain_id <= 5; ++drain_id) // NOLINT(readability-magic-numbers)
  {
    shared->max_doc_txn[drain_id] = 4; // NOLINT(readability-magic-numbers)
    for (fsp::txn_id_t i = 0; i < 9; ++i)
    {
      shared->pending[drain_id].push_back(make_txn(i + 1, "x"));
    } // NOLINT(readability-magic-numbers)
    drains.push_back(exporter_drain_cfg_t{.id = drain_id, .name = fmt::format("BANKA{}", drain_id), .max_doc_txn = 4}); // NOLINT
  }

  exporter<test_txn_t, test_qual_t> exp(make_cfg(dirs, drains, 2), test_qual_t{}, *log_ptr, "test");
  demo_cb                           cb(shared, *log_ptr);
  const auto                        res = exp.exec(cb);
  REQUIRE(res.has_value());
  CHECK(res->total_transactions == 45);   // NOLINT(readability-magic-numbers) -- 9 * 5 drains
  CHECK(res->total_documents == 15);      // NOLINT(readability-magic-numbers) -- ceil(9/4)=3 docs/drain * 5
  CHECK(file_count(dirs.target()) == 15); // NOLINT(readability-magic-numbers)
}

TEST_CASE("exporter surfaces a fatal error from compute_drain_stat without hanging any other phase-1 thread", "[exporter][negative]")
{
  // Confirms the fix documented in exporter_worker.hpp's own fail()/run_phase1() doc comments:
  // a phase-1 failure on ONE thread must still let every OTHER phase-1 thread's own
  // phase1_done() check see phase 1 as complete, instead of hanging forever waiting for the
  // failed thread's own mark_phase1_worker_done() call that would otherwise never come. If this
  // regressed, this test case would hang (timeout) rather than fail an assertion.
  const temp_dir_guard dirs;
  const auto           log_ptr = make_silent_logger();

  auto                              shared = std::make_shared<shared_fixture>();
  std::vector<exporter_drain_cfg_t> drains;
  for (std::uint8_t drain_id = 1; drain_id <= 4; ++drain_id) // NOLINT(readability-magic-numbers)
  {
    shared->max_doc_txn[drain_id] = 2; // NOLINT(readability-magic-numbers)
    shared->pending[drain_id]     = {make_txn(1, "a"), make_txn(2, "b")};
    drains.push_back(exporter_drain_cfg_t{.id = drain_id, .name = fmt::format("BANKA{}", drain_id), .max_doc_txn = 2}); // NOLINT
  }
  shared->fail_compute_drain_stat = true; // every compute_drain_stat() call fails -- worst case for the hang scenario

  exporter<test_txn_t, test_qual_t> exp(make_cfg(dirs, drains, 4), test_qual_t{}, *log_ptr, "test");
  demo_cb                           cb(shared, *log_ptr);
  const auto                        res = exp.exec(cb);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code() == exp_error::fetch_run_stat_failed);
}
