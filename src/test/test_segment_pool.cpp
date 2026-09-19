#include "segment_pool.hpp"
#include <catch2/catch_test_macros.hpp>
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using fsp::segment_pool;
using fsp::segment_result;
using fsp::xml_segment;

// The specific literals below (pool sizes, thread counts) are arbitrary test fixtures, not
// meaningful constants - naming each one would only add indirection, so readability-magic-numbers
// is suppressed for the whole file.
// NOLINTBEGIN(readability-magic-numbers)

namespace
{
  logger::logger_config silent_log_cfg()
  { return logger::logger_config{.console_level = logger::level::off, .file_level = logger::level::off}; }

  std::unique_ptr<logger::Logger> make_silent_logger()
  {
    auto log_ptr = logger::Logger::create(silent_log_cfg());
    REQUIRE(log_ptr.has_value());
    return std::move(*log_ptr);
  }
} // namespace

TEST_CASE("segment_pool::acquire_slot()/set_segment()/set_result() round-trip a single slot", "[segment_pool][positive]")
{
  const auto    log_ptr = make_silent_logger();
  segment_pool  pool(*log_ptr, 8);
  const std::size_t idx = pool.acquire_slot(0);
  pool.set_segment(idx, xml_segment(42, 3, 7, 100, 10, {}, {}));
  pool.set_result(idx, segment_result(42, 3, 7));
  CHECK(pool.segment_at(idx).id() == 42);
  CHECK(pool.segment_at(idx).subtree_type() == 3);
  CHECK(pool.result_at(idx).seg_id() == 42);
  CHECK(pool.result_at(idx).seg_type() == 3);
}

TEST_CASE("segment_pool::acquire_slot() hands out distinct, never-before-used indices when the free queue is empty",
          "[segment_pool][positive]")
{
  const auto   log_ptr = make_silent_logger();
  segment_pool pool(*log_ptr, 4);
  const auto   a = pool.acquire_slot(0);
  const auto   b = pool.acquire_slot(1);
  const auto   c = pool.acquire_slot(2);
  CHECK(a != b);
  CHECK(b != c);
  CHECK(a != c);
}

// Regression test for the segments_/results_ parity bug: acquire_slot() used to call
// segments_.fetch() and results_.fetch() as two INDEPENDENT blocked_vector<T>::fetch() calls,
// each advancing its own atomic size_ counter. Nothing enforced the two counters staying in
// lockstep other than both calls always running in the same order for a single acquire_slot()
// invocation - a correctness argument that holds for one thread at a time, not a guarantee any
// type system or synchronization primitive enforced. Replacing the pair with one
// blocked_vector<segment_slot> and a single fetch() call removes the two-counter question
// entirely: there is exactly one atomic counter, so there is nothing left to diverge.
//
// This test drives many threads concurrently through acquire_slot() (the only path that can
// grow the pool past its free_queues_ reuse pool) and checks that every returned index is
// distinct and that each slot's segment_at()/result_at() halves - written independently, at
// different times, by set_segment() and set_result() respectively, exactly as the real cutter/
// worker pipeline does - observe the values that were written for THAT SAME index, never a
// torn or mismatched pair from another thread's slot.
TEST_CASE("segment_pool::acquire_slot() is safe to call concurrently from many threads", "[segment_pool][positive][concurrency]")
{
  constexpr std::size_t num_threads          = 16;
  constexpr std::size_t acquires_per_thread   = 2048;
  constexpr std::size_t total_acquires        = num_threads * acquires_per_thread;

  const auto   log_ptr = make_silent_logger();
  segment_pool pool(*log_ptr, total_acquires);

  std::vector<std::atomic<int>> seen(total_acquires);
  for (auto& s : seen) s.store(0, std::memory_order_relaxed);

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (std::size_t t = 0; t < num_threads; ++t)
  {
    threads.emplace_back(
      [&pool, &seen, t]
      {
        for (std::size_t i = 0; i < acquires_per_thread; ++i)
        {
          const std::size_t segment_id = t * acquires_per_thread + i;
          const std::size_t idx        = pool.acquire_slot(segment_id);
          pool.set_segment(idx, xml_segment(segment_id, 1, 0, 0, 1, {}, {}));
          pool.set_result(idx, segment_result(segment_id, 1, 0));
          // Both halves of this slot must reflect THIS thread's own write - a parity bug
          // between two independent fetch() counters would surface here as segment_at(idx)
          // and result_at(idx) belonging to what acquire_slot() intended as two different
          // slots.
          CHECK(pool.segment_at(idx).id() == segment_id);
          CHECK(pool.result_at(idx).seg_id() == segment_id);
          seen.at(idx).fetch_add(1, std::memory_order_relaxed);
        }
      });
  }
  for (auto& th : threads) th.join();

  std::size_t duplicates = 0;
  std::size_t gaps       = 0;
  for (const auto& s : seen)
  {
    const auto count = s.load(std::memory_order_relaxed);
    if (count == 0) ++gaps;
    else if (count > 1) ++duplicates;
  }
  CHECK(duplicates == 0);
  CHECK(gaps == 0);
}

// NOLINTEND(readability-magic-numbers)
