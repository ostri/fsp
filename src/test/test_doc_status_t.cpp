// test_doc_status_t.cpp
//
// Direct, pipeline-free unit tests for fsp::doc_status_t (see doc_dscr.hpp) -- specifically its
// try_start_closing() one-shot completion gate, now sitting on FOUR independent facts (syntax_/
// valid_/semantic_/stored_) instead of the original three. The property under test: exactly ONE
// of the four set_*() calls -- whichever one happens to be the LAST to complete the object -- may
// ever see try_start_closing() return true, regardless of the order the four calls arrive in.
#include "doc_dscr.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <array>
#include <tuple> // std::ignore
#include <vector>

using fsp::doc_status_t;
using fsp::three_state;

namespace
{
  // One of the four ways to complete doc_status_t, all with a positive (ok) verdict where the
  // underlying set_*() call takes one -- set_stored() has no ok parameter at all (see its own doc
  // comment in doc_dscr.hpp: storage-completeness has no failure state to report), so this simply
  // ignores that a priori for the stored slot.
  enum class fact : std::uint8_t
  {
    syntax,
    valid,
    semantic,
    stored
  };

  // Applies one fact to status, returning whatever the underlying set_*() call returned (the
  // "did THIS call win try_start_closing()" answer).
  bool apply(doc_status_t& status, fact f)
  {
    switch (f)
    {
    case fact::syntax: return status.set_syntax(true);
    case fact::valid: return status.set_valid(true);
    case fact::semantic: return status.set_semantic(true);
    case fact::stored: return status.set_stored();
    }
    return false; // unreachable -- silences -Wswitch-enum's own "control reaches end" warning path
  }

  // Applies order's four facts, in order, to a fresh doc_status_t, and asserts exactly the LAST
  // call won try_start_closing() -- split out of the TEST_CASE below purely to keep its own
  // cognitive complexity down, not meant to be called from anywhere else.
  void check_one_permutation(const std::array<fact, 4>& order)
  {
    CAPTURE(order);
    doc_status_t status;
    int          winners       = 0;
    bool         last_call_won = false;
    for (const fact f : order)
    {
      last_call_won = apply(status, f);
      if (last_call_won) ++winners;
    }
    CHECK(winners == 1);         // exactly one call, across the whole sequence, won
    CHECK(last_call_won);        // and it was the LAST call in THIS particular order
    CHECK(status.is_finished()); // all four facts now known
    CHECK(status.ok());          // every VERDICT-bearing fact (syntax/valid/semantic) was true
  }
} // namespace

// NOLINTBEGIN(readability-magic-numbers) -- arbitrary test fixture literals (orderings, indices)

TEST_CASE("doc_status_t: try_start_closing() returns true for exactly the call that supplies the 4th (last) fact, "
          "across every ordering of the four set_*() calls",
          "[doc_status_t][positive]")
{
  std::array<fact, 4> order = {fact::syntax, fact::valid, fact::semantic, fact::stored};
  // std::ranges::next_permutation over the 4 facts enumerates all 4! = 24 possible arrival orders
  // -- exhaustive, not a sample, since there are few enough to just enumerate them all outright.
  std::ranges::sort(order);
  int permutations_seen = 0;
  do
  {
    check_one_permutation(order);
    ++permutations_seen;
  } while (std::ranges::next_permutation(order).found);
  CHECK(permutations_seen == 24); // 4! -- confirms the loop above really was exhaustive
}

TEST_CASE("doc_status_t: stored arriving FIRST never wins try_start_closing() on its own", "[doc_status_t][positive]")
{
  doc_status_t status;
  CHECK_FALSE(status.set_stored()); // 1 of 4 facts known -- not finished yet
  CHECK_FALSE(status.is_finished());
  CHECK(status.stored_status() == three_state::valid);
  CHECK(status.ok() == false); // status()/ok() don't consider stored_ at all -- still "unknown" territory otherwise
}

TEST_CASE("doc_status_t: stored arriving LAST is the one that wins try_start_closing()", "[doc_status_t][positive]")
{
  doc_status_t status;
  CHECK_FALSE(status.set_syntax(true));
  CHECK_FALSE(status.set_valid(true));
  CHECK_FALSE(status.set_semantic(true));
  CHECK(status.set_stored()); // the 4th and final fact -- this call, and only this one, wins
  CHECK(status.is_finished());
  CHECK(status.ok());
}

TEST_CASE("doc_status_t: a single invalid verdict short-circuits completion without waiting for stored_", "[doc_status_t][negative]")
{
  doc_status_t status;
  // syntax failing drags done_ straight to k_done_threshold (see set_field()'s own doc comment) --
  // the document is already finished/known-bad even though valid_/semantic_/stored_ never reported.
  CHECK(status.set_syntax(false)); // this call alone wins try_start_closing()
  CHECK(status.is_finished());
  CHECK_FALSE(status.ok());
  CHECK(status.syntax_status() == three_state::invalid);
  CHECK(status.valid_status() == three_state::unknown);
  CHECK(status.semantic_status() == three_state::unknown);
  CHECK(status.stored_status() == three_state::unknown); // never reported -- short-circuit didn't need it
}

TEST_CASE("doc_status_t: try_start_closing() never returns true twice for the same object", "[doc_status_t][negative]")
{
  doc_status_t status;
  CHECK_FALSE(status.set_syntax(true));
  CHECK_FALSE(status.set_valid(true));
  CHECK_FALSE(status.set_semantic(true));
  CHECK(status.set_stored()); // wins once
  // Calling try_start_closing() again (directly, or via any further set_*() on an already-finished
  // object) must never again report true -- closing_ is a sticky, one-shot latch (see its own doc
  // comment in doc_dscr.hpp).
  CHECK_FALSE(status.try_start_closing());
}

TEST_CASE("doc_status_t: stored_status() only ever reaches unknown or valid, never invalid", "[doc_status_t][positive]")
{
  doc_status_t status;
  CHECK(status.stored_status() == three_state::unknown);
  std::ignore = status.set_stored();
  CHECK(status.stored_status() == three_state::valid);
  // set_stored() takes no ok parameter at all -- there is no code path that could ever move
  // stored_ to three_state::invalid (see set_stored()'s own doc comment: storage-completeness has
  // no failure verdict of its own to report).
}
// NOLINTEND(readability-magic-numbers)
