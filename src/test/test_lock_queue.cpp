#include "lock_queue.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using fsp::lock_queue;
using fsp::queue_status;

namespace
{
  constexpr auto wait_timeout = std::chrono::milliseconds(500);
} // namespace

// --- push(const T&) / push(T&&) -----------------------------------------------------

TEST_CASE("lock_queue::push(const T&) adds a copy that pop() returns", "[lock_queue][positive]")
{
  lock_queue<std::string> q;
  const std::string       value = "copied";
  q.push(value);
  CHECK(q.size() == 1);
  std::string out;
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out == "copied");
  // original is untouched by a const& push
  CHECK(value == "copied");
}

TEST_CASE("lock_queue::push(const T&) called zero times leaves the queue empty", "[lock_queue][negative]")
{
  const lock_queue<std::string> q;
  CHECK(q.size() == 0);
  CHECK(q.size_approx() == 0);
}

TEST_CASE("lock_queue::push(T&&) adds a moved-from element that pop() returns", "[lock_queue][positive]")
{
  lock_queue<std::string> q;
  std::string              value = "moved";
  q.push(std::move(value));
  CHECK(q.size() == 1);
  std::string out;
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out == "moved");
}

TEST_CASE("lock_queue::push(T&&) of an empty string still counts as one element", "[lock_queue][negative]")
{
  // "Negative" edge case for push: an empty-but-valid payload must not be treated
  // as "nothing pushed" -- size() must still reflect one queued element.
  lock_queue<std::string> q;
  q.push(std::string{});
  CHECK(q.size() == 1);
  std::string out = "sentinel";
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out.empty());
}

// --- pop -------------------------------------------------------------------------------

TEST_CASE("lock_queue::pop returns active and the front element when data is available", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.push(1);
  q.push(2);
  int out = 0;
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out == 1); // FIFO order
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out == 2);
}

TEST_CASE("lock_queue::pop returns finished once drained after set_finished", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.set_finished();
  int out = -1;
  CHECK(q.pop(out) == queue_status::finished);
  CHECK(out == -1); // untouched: no element was ever assigned
}

TEST_CASE("lock_queue::pop returns aborted immediately even with items still queued", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.push(42);
  q.set_abort();
  int out = -1;
  CHECK(q.pop(out) == queue_status::aborted);
  CHECK(out == -1); // aborted takes priority over available data
}

TEST_CASE("lock_queue::pop blocks until an element is pushed from another thread", "[lock_queue][positive]")
{
  lock_queue<int> q;
  int             out = 0;
  std::thread     producer(
    [&q]
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      q.push(7);
    });
  CHECK(q.pop(out) == queue_status::active);
  CHECK(out == 7);
  producer.join();
}

// --- try_pop -----------------------------------------------------------------------------

TEST_CASE("lock_queue::try_pop returns the element when the queue is non-empty", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.push(5);
  auto result = q.try_pop();
  REQUIRE(result.has_value());
  CHECK(*result == 5);
}

TEST_CASE("lock_queue::try_pop returns unexpected(empty) on an active empty queue", "[lock_queue][negative]")
{
  const lock_queue<int> q;
  // const lock_queue: try_pop is non-const on the API but the empty/active check
  // doesn't require a non-const queue in this scenario -- construct a fresh mutable one.
  lock_queue<int> q2;
  auto            result = q2.try_pop();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == queue_status::empty);
}

TEST_CASE("lock_queue::try_pop returns unexpected(finished) on a drained finished queue", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.set_finished();
  auto result = q.try_pop();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == queue_status::finished);
}

TEST_CASE("lock_queue::try_pop returns unexpected(aborted) even with queued items", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.push(1);
  q.set_abort();
  auto result = q.try_pop();
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error() == queue_status::aborted);
}

// --- set_finished / set_abort --------------------------------------------------------------

TEST_CASE("lock_queue::set_finished transitions an active queue to finished", "[lock_queue][positive]")
{
  lock_queue<int> q;
  CHECK(q.is_active());
  q.set_finished();
  CHECK(q.is_finished());
  CHECK(q.state() == queue_status::finished);
}

TEST_CASE("lock_queue::set_finished called twice in a row stays finished (idempotent)", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.set_finished();
  q.set_finished();
  CHECK(q.is_finished());
}

TEST_CASE("lock_queue::set_abort transitions an active queue to aborted", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.set_abort();
  CHECK(q.is_aborted());
  CHECK(q.state() == queue_status::aborted);
}

TEST_CASE("lock_queue::set_abort after set_finished overrides to aborted", "[lock_queue][negative]")
{
  // Neither method guards against overriding the other -- set_abort always wins if
  // called last, which downstream pop()/try_pop() logic explicitly relies on
  // (aborted is checked before finished).
  lock_queue<int> q;
  q.set_finished();
  q.set_abort();
  CHECK(q.is_aborted());
  CHECK_FALSE(q.is_finished());
}

// --- state / is_finished / is_aborted / is_active -----------------------------------------

TEST_CASE("lock_queue::is_active is true for a freshly constructed queue", "[lock_queue][positive]")
{
  const lock_queue<int> q;
  CHECK(q.is_active());
  CHECK_FALSE(q.is_finished());
  CHECK_FALSE(q.is_aborted());
}

TEST_CASE("lock_queue::is_active is false once finished", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.set_finished();
  CHECK_FALSE(q.is_active());
}

// --- size / size_approx --------------------------------------------------------------------

TEST_CASE("lock_queue::size reflects the number of pushed elements", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.push(1);
  q.push(2);
  q.push(3);
  CHECK(q.size() == 3);
}

TEST_CASE("lock_queue::size is zero right after construction", "[lock_queue][negative]")
{
  const lock_queue<int> q;
  CHECK(q.size() == 0);
}

TEST_CASE("lock_queue::size_approx matches size for single-threaded push/pop", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.push(1);
  q.push(2);
  CHECK(q.size_approx() == 2);
  int out = 0;
  q.pop(out);
  CHECK(q.size_approx() == 1);
}

TEST_CASE("lock_queue::size_approx is zero for an empty queue", "[lock_queue][negative]")
{
  const lock_queue<int> q;
  CHECK(q.size_approx() == 0);
}

// --- drained ---------------------------------------------------------------------------------

TEST_CASE("lock_queue::drained is true once finished and empty", "[lock_queue][positive]")
{
  lock_queue<int> q;
  q.push(1);
  int out = 0;
  q.pop(out);
  q.set_finished();
  CHECK(q.drained());
}

TEST_CASE("lock_queue::drained is false when finished but items remain", "[lock_queue][negative]")
{
  lock_queue<int> q;
  q.push(1);
  q.set_finished();
  CHECK_FALSE(q.drained());
}

TEST_CASE("lock_queue::drained is false while merely active and empty", "[lock_queue][negative]")
{
  const lock_queue<int> q;
  CHECK_FALSE(q.drained());
}

// --- push_range ------------------------------------------------------------------------------

TEST_CASE("lock_queue::push_range enqueues every element of a non-empty range in order", "[lock_queue][positive]")
{
  lock_queue<int>      q;
  std::array<int, 3> values{10, 20, 30};
  q.push_range(values);
  CHECK(q.size() == 3);
  int out = 0;
  q.pop(out);
  CHECK(out == 10);
  q.pop(out);
  CHECK(out == 20);
  q.pop(out);
  CHECK(out == 30);
}

TEST_CASE("lock_queue::push_range on an empty range leaves the queue unchanged", "[lock_queue][negative]")
{
  lock_queue<int>      q;
  std::vector<int> empty_range;
  q.push_range(empty_range);
  CHECK(q.size() == 0);
  CHECK(q.size_approx() == 0);
}
