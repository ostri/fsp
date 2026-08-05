#include "exporter_state.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <set>
#include <thread>
#include <vector>

using fsp::drain_dscr_t;
using fsp::exporter_error;
using fsp::exporter_error_info;
using fsp::exporter_state;
using fsp::run_stat_pair_t;

namespace
{
  std::vector<drain_dscr_t> two_drains()
  {
    return {
      drain_dscr_t{.id = 1, .name = "BANKA1", .dscr = "Banka Ena", .max_doc_txn = 100}, // NOLINT(readability-magic-numbers)
      drain_dscr_t{.id = 2, .name = "BANKA2", .dscr = "Banka Dva", .max_doc_txn = 50},  // NOLINT(readability-magic-numbers)
    };
  }

  std::vector<drain_dscr_t> one_drain()
  {
    return {drain_dscr_t{.id = 1, .name = "BANKA1", .dscr = "Banka Ena", .max_doc_txn = 100}}; // NOLINT(readability-magic-numbers)
  }
} // namespace

// --- construction --------------------------------------------------------------------------

TEST_CASE("exporter_state construction seeds available_drains_ with every configured drain", "[exporter_state][positive]")
{
  const exporter_state state(two_drains());
  CHECK_FALSE(state.available_drains_empty());
  CHECK(state.find_drain(1) != nullptr);
  CHECK(state.find_drain(2) != nullptr);
  CHECK(state.find_drain(99) == nullptr); // NOLINT(readability-magic-numbers)
}

TEST_CASE("exporter_state construction with an empty drain list starts already exhausted", "[exporter_state][negative]")
{
  const exporter_state state({});
  CHECK(state.available_drains_empty());
}

// --- pick_or_keep_drain --------------------------------------------------------------------

TEST_CASE("pick_or_keep_drain returns the current drain unchanged without consulting available_drains_", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     picked = state.pick_or_keep_drain(42); // NOLINT(readability-magic-numbers) -- not even a real drain id
  REQUIRE(picked.has_value());
  CHECK(*picked == 42); // NOLINT(readability-magic-numbers,bugprone-unchecked-optional-access) -- REQUIRE above guards this
}

TEST_CASE("pick_or_keep_drain(nullopt) on a single-drain state deterministically returns that drain", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     picked = state.pick_or_keep_drain(std::nullopt);
  REQUIRE(picked.has_value());
  CHECK(*picked == 1); // NOLINT(bugprone-unchecked-optional-access) -- REQUIRE above guards this
}

TEST_CASE("pick_or_keep_drain(nullopt) on a multi-drain state returns one of the configured ids", "[exporter_state][positive]")
{
  exporter_state state(two_drains());
  for (int i = 0; i < 20; ++i) // NOLINT(readability-magic-numbers) -- exercise the random branch a few times
  {
    const auto picked = state.pick_or_keep_drain(std::nullopt);
    REQUIRE(picked.has_value());
    CHECK((*picked == 1 || *picked == 2)); // NOLINT(bugprone-unchecked-optional-access) -- REQUIRE above guards this
  }
}

TEST_CASE("pick_or_keep_drain(nullopt) on an exhausted state returns nullopt", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  state.remove_available_drain(1);
  const auto picked = state.pick_or_keep_drain(std::nullopt);
  CHECK_FALSE(picked.has_value());
}

// --- remove_available_drain / available_drains_empty ---------------------------------------

TEST_CASE("remove_available_drain removes exactly the requested drain", "[exporter_state][positive]")
{
  exporter_state state(two_drains());
  state.remove_available_drain(1);
  CHECK_FALSE(state.available_drains_empty());
  const auto picked = state.pick_or_keep_drain(std::nullopt);
  REQUIRE(picked.has_value());
  CHECK(*picked == 2); // NOLINT(bugprone-unchecked-optional-access) -- REQUIRE above guards this
}

TEST_CASE("remove_available_drain down to zero leaves available_drains_empty true", "[exporter_state][positive]")
{
  exporter_state state(two_drains());
  state.remove_available_drain(1);
  state.remove_available_drain(2);
  CHECK(state.available_drains_empty());
}

TEST_CASE("remove_available_drain on an unknown or already-removed id is a harmless no-op", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  state.remove_available_drain(1);
  CHECK_NOTHROW(state.remove_available_drain(1));  // already removed
  CHECK_NOTHROW(state.remove_available_drain(99)); // NOLINT(readability-magic-numbers) -- never existed
  CHECK(state.available_drains_empty());
}

// --- is_drain_loaded / load_drain_stat_if_needed ------------------------------------------

TEST_CASE("is_drain_loaded is false before load_drain_stat_if_needed is ever called", "[exporter_state][negative]")
{
  const exporter_state state(one_drain());
  CHECK_FALSE(state.is_drain_loaded(1));
}

TEST_CASE("load_drain_stat_if_needed loads stats exactly once", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  int            call_count = 0;

  const auto fetch = [&call_count]() -> fsp::exp_result<run_stat_pair_t>
  {
    ++call_count;
    return run_stat_pair_t{.remaining_txn_count = 250, .existing_doc_count = 3}; // NOLINT(readability-magic-numbers)
  };

  state.load_drain_stat_if_needed(1, 100, fetch); // NOLINT(readability-magic-numbers)
  CHECK(state.is_drain_loaded(1));
  CHECK(call_count == 1);

  // A second call (as if another thread lost the race) must not re-invoke fetch.
  state.load_drain_stat_if_needed(1, 100, fetch); // NOLINT(readability-magic-numbers)
  CHECK(call_count == 1);
}

TEST_CASE("load_drain_stat_if_needed leaves the drain unloaded when fetch fails", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  const auto     fetch = []() -> fsp::exp_result<run_stat_pair_t>
  { return std::unexpected(exporter_error_info(exporter_error::fetch_run_stat_failed, "boom")); };

  state.load_drain_stat_if_needed(1, 100, fetch); // NOLINT(readability-magic-numbers)
  CHECK_FALSE(state.is_drain_loaded(1));
}

TEST_CASE("load_drain_stat_if_needed on an unknown drain id is a harmless no-op", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  int            call_count = 0;
  const auto     fetch      = [&call_count]() -> fsp::exp_result<run_stat_pair_t>
  {
    ++call_count;
    return run_stat_pair_t{};
  };
  CHECK_NOTHROW(state.load_drain_stat_if_needed(99, 100, fetch)); // NOLINT(readability-magic-numbers)
  CHECK(call_count == 0);
}

// --- next_doc_id ----------------------------------------------------------------------------

TEST_CASE("next_doc_id starts at existing_doc_count + 1 (resume-after-crash semantics)", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     fetch = []() -> fsp::exp_result<run_stat_pair_t>
  { return run_stat_pair_t{.remaining_txn_count = 10, .existing_doc_count = 5}; }; // NOLINT(readability-magic-numbers)
  state.load_drain_stat_if_needed(1, 100, fetch);                                  // NOLINT(readability-magic-numbers)

  CHECK(state.next_doc_id(1) == 6); // NOLINT(readability-magic-numbers)
  CHECK(state.next_doc_id(1) == 7); // NOLINT(readability-magic-numbers)
}

TEST_CASE("next_doc_id starts at 1 when the drain has no pre-existing documents", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     fetch = []() -> fsp::exp_result<run_stat_pair_t> { return run_stat_pair_t{}; };
  state.load_drain_stat_if_needed(1, 100, fetch); // NOLINT(readability-magic-numbers)
  CHECK(state.next_doc_id(1) == 1);
}

TEST_CASE("next_doc_id allocates unique, non-colliding ids under concurrent callers", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     fetch = []() -> fsp::exp_result<run_stat_pair_t> { return run_stat_pair_t{}; };
  state.load_drain_stat_if_needed(1, 100, fetch); // NOLINT(readability-magic-numbers)

  static constexpr int                    THREADS        = 8;
  static constexpr int                    IDS_PER_THREAD = 1000;
  std::vector<std::vector<std::uint64_t>> per_thread_ids(THREADS);

  {
    std::vector<std::jthread> threads;
    threads.reserve(THREADS);
    for (int t = 0; t < THREADS; ++t)
    {
      threads.emplace_back(
        [&state, &per_thread_ids, t]
        {
          auto& ids = per_thread_ids.at(static_cast<std::size_t>(t));
          ids.reserve(IDS_PER_THREAD);
          for (int i = 0; i < IDS_PER_THREAD; ++i) { ids.push_back(state.next_doc_id(1)); }
        });
    }
  }

  std::set<std::uint64_t> all_ids;
  for (const auto& ids : per_thread_ids) { all_ids.insert(ids.begin(), ids.end()); }
  CHECK(all_ids.size() == static_cast<std::size_t>(THREADS * IDS_PER_THREAD));
}

// --- register_doc_start / finalize_doc / increment_drain_doc_count -------------------------

TEST_CASE("register_doc_start / finalize_doc round-trip a document's bookkeeping entry", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  const auto     ndx = state.register_doc_start(fsp::doc_statistics_t{.doc_name       = "",
                                                                      .txn_count      = 0,
                                                                      .header_written = false,
                                                                      .footer_written = false,
                                                                      .start_ts       = {},
                                                                      .duration       = {},
                                                                      .worker_id      = 3, // NOLINT(readability-magic-numbers)
                                                                      .drain_id       = 1});
  CHECK_NOTHROW(state.finalize_doc(ndx,
                                   fsp::doc_statistics_t{.doc_name       = "out.xml",
                                                         .txn_count      = 10, // NOLINT(readability-magic-numbers)
                                                         .header_written = true,
                                                         .footer_written = true,
                                                         .start_ts       = {},
                                                         .duration       = {},
                                                         .worker_id      = 3, // NOLINT(readability-magic-numbers)
                                                         .drain_id       = 1}));
}

TEST_CASE("finalize_doc on an out-of-range index is a harmless no-op", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  CHECK_NOTHROW(state.finalize_doc(999, fsp::doc_statistics_t{})); // NOLINT(readability-magic-numbers)
}

TEST_CASE("increment_drain_doc_count does not throw for any drain id", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  CHECK_NOTHROW(state.increment_drain_doc_count(1));
  CHECK_NOTHROW(state.increment_drain_doc_count(1));
}

// --- stop_token / request_stop ---------------------------------------------------------------

TEST_CASE("stop_token reflects request_stop across the shared source", "[exporter_state][positive]")
{
  exporter_state state(one_drain());
  CHECK_FALSE(state.stop_token().stop_requested());
  state.request_stop();
  CHECK(state.stop_token().stop_requested());
}

TEST_CASE("request_stop is idempotent", "[exporter_state][negative]")
{
  exporter_state state(one_drain());
  state.request_stop();
  CHECK_NOTHROW(state.request_stop());
  CHECK(state.stop_token().stop_requested());
}
