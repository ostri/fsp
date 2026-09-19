#include "blocked_vector.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
#include <vector>

using fsp::blocked_vector;

// The specific literals below (element counts, thread counts) are arbitrary test fixtures, not
// meaningful constants - naming each one would only add indirection, so readability-magic-numbers
// is suppressed for the whole file.
// NOLINTBEGIN(readability-magic-numbers)

TEST_CASE("blocked_vector::fetch() returns sequential indices and constructs each element",
          "[blocked_vector][positive]")
{
  blocked_vector<int> v(16);
  auto                 a = v.fetch(1);
  auto                 b = v.fetch(2);
  auto                 c = v.fetch(3);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  REQUIRE(c.has_value());
  CHECK(a->first == 0);
  CHECK(b->first == 1);
  CHECK(c->first == 2);
  CHECK(a->second == 1);
  CHECK(b->second == 2);
  CHECK(c->second == 3);
  CHECK(v.size() == 3);
}

TEST_CASE("blocked_vector::fetch() returns nullopt once max_elements is reached", "[blocked_vector][negative]")
{
  blocked_vector<int> v(2);
  CHECK(v.fetch(1).has_value());
  CHECK(v.fetch(2).has_value());
  const auto third = v.fetch(3);
  CHECK_FALSE(third.has_value());
  CHECK(v.size() == 2); // the failed fetch() must not have bumped size() past max_elements
}

TEST_CASE("blocked_vector::operator[] reads back a value fetch() already constructed",
          "[blocked_vector][positive]")
{
  blocked_vector<int> v(8);
  const auto           r = v.fetch(42);
  REQUIRE(r.has_value());
  CHECK(v[r->first] == 42);
  const blocked_vector<int>& cv = v;
  CHECK(cv[r->first] == 42);
}

// Regression test for the data race ThreadSanitizer caught in get_storage(): the old
// implementation's "fast path" read block_ptrs_.size()/block_ptrs_[b] (a plain std::vector<T*>)
// without holding blocks_mutex_, while the "slow path" resized that same vector and wrote to it
// UNDER the lock - a std::vector<T*>::resize() call on one thread can reallocate the vector's own
// backing array while another thread concurrently reads block_ptrs_[b] with no synchronization at
// all, which is undefined behavior regardless of whether it happens to produce a visibly wrong
// value on any given run (ThreadSanitizer flags the race itself, not just its occasional visible
// effect - see docs/internals.md's own writeup of this bug for the exact TSan report that caught
// it, first observed as an intermittent SIGABRT in ach's own fsp::pipeline::doc_data() days after
// blocked_vector started being used from multiple pipeline_worker threads at once).
//
// Fixed by (1) sizing block_ptrs_/raw_blocks_ once, in the constructor, to their final
// max_blocks length (so no resize() ever runs again after construction - indexing block_ptrs_[b]
// itself needs no lock any more), and (2) making each block_ptrs_ element a std::atomic<T*>, not
// a plain T*, so the fast path's load(acquire) happens-before-pairs with the slow path's
// store(release) - the one thing sizing block_ptrs_ up front does not, by itself, make safe.
//
// This test forces many threads to concurrently fetch() enough elements to span several blocks
// (BlockSize elements is not exposed publicly, so this over-provisions generously via a small
// element type and a large element count instead) - every fetch() must return a distinct index
// and a live reference, with no index ever handed out twice and no thread ever observing a
// still-being-constructed element through operator[]. Run under ThreadSanitizer (see fsp's own
// ACH_ENABLE_TSAN-equivalent build option) this is expected to pass cleanly; before the fix above,
// the same run reliably reported the get_storage() data race described here.
TEST_CASE("blocked_vector::fetch() is safe to call concurrently from many threads across several blocks",
          "[blocked_vector][positive][concurrency]")
{
  constexpr std::size_t num_threads          = 16;
  constexpr std::size_t fetches_per_thread    = 4096; // several BlockSize-worth of elements, whatever BlockSize<int> resolves to
  constexpr std::size_t total_fetches         = num_threads * fetches_per_thread;

  blocked_vector<std::size_t> v(total_fetches);
  std::vector<std::atomic<int>> seen(total_fetches); // seen[idx] counts how many times idx was ever handed out
  for (auto& s : seen) s.store(0, std::memory_order_relaxed);

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (std::size_t t = 0; t < num_threads; ++t)
  {
    threads.emplace_back(
      [&v, &seen, t]
      {
        for (std::size_t i = 0; i < fetches_per_thread; ++i)
        {
          auto r = v.fetch(t * fetches_per_thread + i); // value itself is irrelevant, just something to read back
          REQUIRE(r.has_value());
          CHECK(v[r->first] == r->second); // operator[] must observe the just-constructed value, not a torn/stale one
          seen.at(r->first).fetch_add(1, std::memory_order_relaxed);
        }
      });
  }
  for (auto& th : threads) th.join();

  CHECK(v.size() == total_fetches);
  // Every index in [0, total_fetches) must have been handed out to EXACTLY one fetch() call - a
  // duplicate or a gap would both indicate size_'s own fetch_add()/get_storage()'s own block
  // assignment raced each other.
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
