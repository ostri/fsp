// test_run_doc_data.cpp
//
// Tests for run_data()/doc_data() (see run_doc_data.hpp, pipeline_hooks.hpp): that every hook
// concerning a document is actually reached during processing, that fsp::lock() genuinely
// serializes concurrent access to a custom RunData/DocData instance (a second writer must wait
// for the first one to finish, and must then observe the FIRST writer's already-applied update,
// not a stale/torn value), and that the custom RunData/DocData types themselves are the ones the
// factory (pipeline_hooks_crtp<Derived, RunData, DocData>::make_run_data_struct()/
// make_doc_data_struct()) actually constructs -- not silently falling back to the plain
// fsp::run_data_root/fsp::doc_data_root base.
//
// Reuses the same fsp::work/xsd/temp_dir_guard fixture style as test_pipeline_stages.cpp -- see
// that file's own top comment for why a second reflected schema isn't worth building just for
// this.
#include "importer.hpp"
#include "run_doc_data.hpp"
#include "typed_semantic_check.hpp"
#include "work.hpp" // IWYU pragma: keep -- ^^fsp::work needs the actual schema classes
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <logger/logger_config.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
  namespace fs = std::filesystem;

  class temp_dir_guard
  {
  public:
    temp_dir_guard()
    : dir_(fs::temp_directory_path() / ("fsp_run_doc_data_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    { fs::create_directory(dir_); }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)                 = delete;
    temp_dir_guard& operator=(const temp_dir_guard&)      = delete;
    temp_dir_guard(temp_dir_guard&&)                      = delete;
    temp_dir_guard&           operator=(temp_dir_guard&&) = delete;
    [[nodiscard]] std::string write(std::string_view name, std::string_view content) const
    {
      const auto    path = dir_ / name;
      std::ofstream out(path, std::ios::binary);
      out << content;
      out.close();
      return path.string();
    }
  private:
    fs::path                    dir_;
    static inline std::uint32_t counter_ = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
  };

  logger::logger_config silent_log_cfg(std::string_view app_name)
  {
    return logger::logger_config{.app_name = std::string(app_name), .console_level = logger::level::off, .file_level = logger::level::off};
  }

  std::string xsd_path() { return std::string(FSP_TEST_SOURCE_DIR) + "/xsd/pacs.008.xsd"; }

  // NbOfTxs must match the actual number of <CdtTrfTxInf> blocks make_doc() below generates --
  // it's this test's own declared_count (see my_doc_data), read back by on_doc_sem_check() and
  // compared against actual_count (incremented once per real transaction segment).
  std::string make_hdr(int num_txns)
  {
    return fmt::format(R"(<x:GrpHdr>
      <x:MsgId>MSG1</x:MsgId>
      <CreDtTm>2026-05-14T07:41:50.161Z</CreDtTm>
      <NbOfTxs>{}</NbOfTxs>
      <TtlIntrBkSttlmAmt Ccy="EUR">1.00</TtlIntrBkSttlmAmt>
      <IntrBkSttlmDt>2026-05-14</IntrBkSttlmDt>
      <SttlmInf><SttlmMtd>CLRG</SttlmMtd></SttlmInf>
      <PmtTpInf><SvcLvl><Cd>SEPA</Cd></SvcLvl><CtgyPurp><Cd>FCIN</Cd></CtgyPurp></PmtTpInf>
      <InstgAgt><FinInstnId><BICFI>FHQIBWWK</BICFI></FinInstnId></InstgAgt>
      <InstdAgt><FinInstnId><BICFI>KWNRKEQS</BICFI></FinInstnId></InstdAgt>
    </x:GrpHdr>)",
                       num_txns);
  }

  // One transaction template, reused several times per document (with a distinct TxId/E2E each)
  // so a single document has enough P-role segments for multiple worker threads to genuinely
  // process it concurrently -- needed to exercise fsp::lock()'s real mutual exclusion, not just
  // its API.
  std::string txn(int n)
  {
    return fmt::format(R"(<CdtTrfTxInf>
      <PmtId><EndToEndId>E2E{0}</EndToEndId><TxId>TXN{0}</TxId></PmtId>
      <PmtTpInf><SvcLvl><Cd>SEPA</Cd></SvcLvl><CtgyPurp><Cd>FCIN</Cd></CtgyPurp></PmtTpInf>
      <IntrBkSttlmAmt Ccy="EUR">1.00</IntrBkSttlmAmt>
      <ChrgBr>SLEV</ChrgBr>
      <InstgAgt><FinInstnId><BICFI>FHQIBWWK</BICFI></FinInstnId></InstgAgt>
      <InstdAgt><FinInstnId><BICFI>KWNRKEQS</BICFI></FinInstnId></InstdAgt>
      <Dbtr><Nm>DEBTOR</Nm><Id><OrgId><AnyBIC>PINVXDRY</AnyBIC></OrgId></Id></Dbtr>
      <DbtrAcct><Id><IBAN>EE172303863092752160</IBAN></Id></DbtrAcct>
      <DbtrAgt><FinInstnId><BICFI>SSMKBKTSPFP</BICFI></FinInstnId></DbtrAgt>
      <CdtrAgt><FinInstnId><BICFI>EGOKPM2HXPI</BICFI></FinInstnId></CdtrAgt>
      <Cdtr><Nm>CREDITOR</Nm><Id><OrgId><AnyBIC>UOEJUW06TBB</AnyBIC></OrgId></Id></Cdtr>
      <CdtrAcct><Id><IBAN>EE654412207930872869</IBAN></Id></CdtrAcct>
      <RmtInf><Strd><CdtrRefInf><Tp><CdOrPrtry><Cd>SCOR</Cd></CdOrPrtry><Issr>ISO</Issr></Tp><Ref>RF47TLOBX61dcMsQwVZ7ELlg4</Ref></CdtrRefInf></Strd></RmtInf>
    </CdtTrfTxInf>)",
                       n);
  }

  std::string wrap_document(std::string_view hdr, const std::string& txns)
  {
    return fmt::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<Document xmlns="urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"
          xmlns:x="urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"
          xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <FIToFICstmrCdtTrf>
    {}
    {}
  </FIToFICstmrCdtTrf>
</Document>
)",
                       hdr,
                       txns);
  }

  // Document with num_txns transactions -- enough P-role segments for genuine cross-thread
  // contention on the same doc_data() instance. NbOfTxs in the header matches num_txns (see
  // make_hdr()'s own doc comment).
  std::string make_doc(int num_txns)
  {
    std::string txns;
    for (int i = 0; i < num_txns; ++i) txns += txn(i);
    return wrap_document(make_hdr(num_txns), txns);
  }

  static constexpr int k_factory_tag_run = 0x5EED1; // NOLINT(readability-magic-numbers) -- arbitrary, distinct-from-doc marker
  static constexpr int k_factory_tag_doc = 0x5EED2; // NOLINT(readability-magic-numbers) -- arbitrary, distinct-from-run marker

  // --- custom RunData: proves the factory constructs THIS type (factory_tag), and that a value
  // one thread writes under fsp::lock() is visible, complete, to the next thread that locks it --
  // not a torn/stale read. ---
  struct my_run_data : fsp::run_data_root
  {
    int factory_tag = k_factory_tag_run; // set here, not by the framework -- proves make_run_data_struct() really built a my_run_data
    std::size_t total_segments_seen = 0; // updated only under fsp::lock() -- see on_type() below
  };

  // --- custom DocData: same factory-proof shape, plus the counters on_doc_sem_check() compares
  // to prove it observes on_type()'s already-applied update, not a stale one. ---
  struct my_doc_data : fsp::doc_data_root
  {
    int         factory_tag    = k_factory_tag_doc; // set here -- proves make_doc_data_struct() really built a my_doc_data
    std::size_t declared_count = 0;                 // set once by on_type(hdr), read back by on_doc_sem_check()
    std::size_t actual_count   = 0;                 // incremented once per txn segment by on_type(txn), under fsp::lock()
    void        reset() override
    {
      declared_count = 0;
      actual_count   = 0;
      fsp::doc_data_root::reset();
    }
  };

  // --- Survives past exec() returning, unlike run_data()/doc_data() themselves (run_data_ is
  // destroyed right after on_run_end() returns; a document's doc_data() slot is recycled right
  // after on_doc_finish() returns -- see pipeline.cpp) -- every hook below copies whatever it
  // needs to assert on afterwards into this, INSIDE its own fsp::lock() critical section so the
  // copy itself reflects a consistent, non-torn snapshot. This is the mechanism the tests use to
  // check factory_tag/call order/counter values post-run, since the originals are gone by then.
  struct observed_state
  {
    std::mutex               mtx; // guards every field below -- hook_call_order/collected during genuinely concurrent hook calls
    std::vector<std::string> hook_call_order;
    int                      run_data_factory_tag_seen = 0;
    int                      doc_data_factory_tag_seen = 0;
    std::size_t              declared_count_seen       = 0;
    std::size_t actual_count_at_sem_check      = 0; // what on_doc_sem_check() itself observed (see the mismatch-detection test below)
    std::size_t total_segments_seen_at_run_end = 0; // run_data()->total_segments_seen, copied out in on_run_end()
    bool        run_start_seen                 = false;
    bool        run_end_seen                   = false;

    void log_call(const std::string& name)
    {
      const std::lock_guard lk(mtx);
      hook_call_order.push_back(name);
    }
  };

  class run_doc_data_test_hooks
  : public fsp::typed_semantic_check<run_doc_data_test_hooks, ^^fsp::work, fsp::seg_schema, my_run_data, my_doc_data>
  {
  public:
    // seg_delay: an artificial per-segment pause inside on_type(), so multiple worker threads
    // processing this document's several transactions concurrently are provably still inside
    // fsp::lock()'s critical section at the same time as each other -- without this, a fast
    // machine could serialize P work incidentally and never actually exercise the lock's mutual
    // exclusion.
    run_doc_data_test_hooks(std::shared_ptr<observed_state> observed, std::chrono::milliseconds seg_delay)
    : observed_(std::move(observed))
    , seg_delay_(seg_delay)
    {
    }
  protected:
    void on_run_start(const fsp::doc_set_dscr& /*ds_dscr*/) override
    {
      auto guard = fsp::lock(run_data());
      // factory_tag is checked here, not after the run -- run_data_ itself is destroyed right
      // after on_run_end() returns (see pipeline::process_files()), so this is the only window in
      // which the LIVE object (not a copy) can be inspected at all.
      REQUIRE(guard->factory_tag == k_factory_tag_run);
      const std::lock_guard lk(observed_->mtx);
      observed_->run_start_seen            = true;
      observed_->run_data_factory_tag_seen = guard->factory_tag;
      observed_->hook_call_order.push_back("on_run_start");
    }
    void on_run_end(const fsp::doc_set_counter& /*counters*/,
                    const fsp::doc_set_dscr& /*ds_dscr*/,
                    std::span<const fsp::pipeline_hooks*> /*worker_clones*/) override
    {
      auto guard = fsp::lock(run_data());
      REQUIRE(guard->factory_tag == k_factory_tag_run);
      const std::lock_guard lk(observed_->mtx);
      observed_->run_end_seen                   = true;
      observed_->total_segments_seen_at_run_end = guard->total_segments_seen;
      observed_->hook_call_order.push_back("on_run_end");
    }
    void on_wrk_start(int /*worker_id*/, fsp::cstr_t /*thread_name*/) override { observed_->log_call("on_wrk_start"); }
    void on_wrk_end(int /*worker_id*/, fsp::cstr_t /*thread_name*/) override { observed_->log_call("on_wrk_end"); }
    void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& /*dscr*/) override
    {
      auto guard = fsp::lock(doc_data(doc_ndx));
      REQUIRE(guard->factory_tag == k_factory_tag_doc);
      const std::lock_guard lk(observed_->mtx);
      observed_->doc_data_factory_tag_seen = guard->factory_tag;
      observed_->hook_call_order.push_back("on_doc_open");
    }
    void on_doc_cutting_finished(std::size_t /*doc_ndx*/, const fsp::doc_dscr& /*dscr*/) override
    { observed_->log_call("on_doc_cutting_finished"); }
    bool on_doc_sem_check(std::size_t doc_ndx) override
    {
      auto guard = fsp::lock(doc_data(doc_ndx));
      {
        const std::lock_guard lk(observed_->mtx);
        observed_->hook_call_order.push_back("on_doc_sem_check");
        // This is the cross-hook "did B see A's fully-applied update" proof: every on_type()
        // call for this document has already run and applied its own fsp::lock()-protected
        // increment by the time this fires (doc_counters' "all segments processed" completion
        // gates on_doc_sem_check(), see doc_counters.hpp) -- copying actual_count here, under the
        // SAME fsp::lock() critical section that reads it for the verdict below, lets the test
        // assert afterwards that this hook (B) really observed on_type(txn)'s (A's) writes, not a
        // value from before any of them ran.
        observed_->actual_count_at_sem_check = guard->actual_count;
        observed_->declared_count_seen       = guard->declared_count;
      }
      return guard->actual_count == guard->declared_count;
    }
    bool on_doc_close(std::size_t /*doc_ndx*/,
                      const fsp::doc_status_t& /*verdict*/,
                      const fsp::error_info& /*err*/,
                      const fsp::doc_dscr& /*dscr*/) override
    {
      observed_->log_call("on_doc_close");
      return true;
    }
    void on_doc_finish(std::size_t /*doc_ndx*/) override { observed_->log_call("on_doc_finish"); }
  public:
    // Not an override point (typed_semantic_check dispatches per schema class) -- both on_type()
    // overloads funnel through this so declared_count/actual_count stay in one place.
    [[nodiscard]] bool on_type(const fsp::work::pacs8_hdr& hdr, fsp::segment_result& result, bool /*is_first*/, bool /*is_last*/) const
    {
      {
        auto guard            = fsp::lock(doc_data(result.doc_ndx()));
        guard->declared_count = static_cast<std::size_t>(hdr.no_of_txn);
      }
      observed_->log_call("on_type(hdr)");
      return true;
    }
    [[nodiscard]] bool on_type(const fsp::work::pacs8_txn& /*txn*/, fsp::segment_result& result, bool /*is_first*/, bool /*is_last*/) const
    {
      // The artificial delay sits INSIDE the lock, on purpose -- this is what forces a second
      // concurrent thread to genuinely block on fsp::lock() (mutual exclusion under real
      // contention), rather than the two threads' critical sections happening to never overlap.
      {
        auto guard = fsp::lock(doc_data(result.doc_ndx()));
        if (seg_delay_.count() > 0) std::this_thread::sleep_for(seg_delay_);
        guard->actual_count += 1;
      }
      {
        auto guard = fsp::lock(run_data());
        guard->total_segments_seen += 1;
      }
      observed_->log_call("on_type(txn)");
      return true;
    }
  private:
    std::shared_ptr<observed_state> observed_;
    std::chrono::milliseconds       seg_delay_;
  };

  fsp::importer_config make_cfg(std::string_view app_name, std::size_t num_of_workers)
  {
    return fsp::importer_config{.targets        = fsp::proc_data_of<^^fsp::work>(),
                                .num_of_workers = num_of_workers,
                                .log_config     = silent_log_cfg(app_name),
                                .program_name   = std::string(app_name)};
  }
} // namespace

// NOLINTBEGIN(readability-magic-numbers) -- arbitrary test fixture literals (delays, counts)

// --- Test 1: every hook that concerns a document actually fires while it is processed, the
// factory really built the CUSTOM RunData/DocData types (not the plain base), and the relative
// order matches the documented lifecycle (open before any on_type, sem_check only after every
// on_type, close after sem_check, finish last of all). -------------------------------------------
// NOLINTBEGIN(readability-function-cognitive-complexity) -- one long, linear sequence of
// independent CHECKs (hook presence, ordering, factory tags) is more readable as a single test
// case than split across several that would each need to re-run the whole pipeline just to check
// one more fact about the SAME single run.
TEST_CASE("run_doc_data: every on_* hook fires during processing, using the factory-built custom types", "[pipeline][run_doc_data][hooks]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", make_doc(3));

  auto                    observed = std::make_shared<observed_state>();
  run_doc_data_test_hooks hooks(observed, std::chrono::milliseconds(0));
  auto                    cfg = make_cfg("test-hooks-fire", 2);
  auto [p, res]               = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  CHECK(res->total_docs() == 1);
  CHECK(p->ds_dscr()[0].status().ok());
  CHECK(p->get_results().size() == 4); // 1 header + 3 transactions, all processed successfully
  CHECK(p->get_errors().empty());

  // The factory (pipeline_hooks_crtp<Derived, RunData, DocData>::make_run_data_struct()/
  // make_doc_data_struct()) really constructed my_run_data/my_doc_data, not the plain
  // fsp::run_data_root/fsp::doc_data_root base -- had it fallen back to the base, factory_tag
  // wouldn't exist at all (compile error) or, if RunData/DocData themselves were wrong, would
  // read as 0 (my_run_data/my_doc_data's own in-class initializer never ran).
  CHECK(observed->run_data_factory_tag_seen == k_factory_tag_run);
  CHECK(observed->doc_data_factory_tag_seen == k_factory_tag_doc);
  CHECK(observed->run_start_seen);
  CHECK(observed->run_end_seen);

  // Every hook concerning this document/run fired at least once.
  const auto& order = observed->hook_call_order;
  for (const char* expected : {"on_run_start",
                               "on_wrk_start",
                               "on_doc_open",
                               "on_type(hdr)",
                               "on_type(txn)",
                               "on_doc_cutting_finished",
                               "on_doc_sem_check",
                               "on_doc_close",
                               "on_doc_finish",
                               "on_wrk_end",
                               "on_run_end"})
  {
    INFO("expected hook missing from call order: " << expected);
    CHECK(std::find(order.begin(), order.end(), expected) != order.end());
  }

  // Relative order -- each of these hooks fires exactly once for a single-document, single-worker
  // run, so their positions in the shared log are directly comparable.
  auto pos = [&](const std::string& name) { return std::distance(order.begin(), std::find(order.begin(), order.end(), name)); };
  CHECK(pos("on_run_start") < pos("on_doc_open"));
  CHECK(pos("on_doc_open") < pos("on_type(hdr)"));
  CHECK(pos("on_type(hdr)") < pos("on_doc_sem_check")); // declared_count must be set before the count comparison runs
  CHECK(pos("on_doc_sem_check") < pos("on_doc_close"));
  CHECK(pos("on_doc_close") < pos("on_doc_finish")); // on_doc_finish is the documented "fires right after on_doc_close" hook
  CHECK(pos("on_doc_finish") < pos("on_run_end"));   // doc-level work is fully done before the run itself ends

  // Cross-hook data flow: on_doc_sem_check() (hook B) observed on_type(txn)'s (hook A's) fully
  // applied writes, not a stale value from before any of them ran.
  CHECK(observed->actual_count_at_sem_check == 3);
  CHECK(observed->declared_count_seen == 3);
}
// NOLINTEND(readability-function-cognitive-complexity)

// --- Test 2: two independent worker threads racing to lock the SAME run_data() instance --
// fsp::lock() must genuinely force the second one to wait for the first to finish (measured via
// wall-clock time: if the delay inside the critical section were NOT serialized, concurrent
// threads would overlap their sleeps and the whole run would finish much faster than "N holders *
// delay each"), and the value each successive holder sees must be the FULL, already-applied
// result of every earlier holder's write (checked via the exact final count, not just "some
// count"). ------------------------------------------------------------------------------------
TEST_CASE("run_doc_data: a second on_type() call must wait for fsp::lock() held by a concurrent one, then sees its result",
          "[pipeline][run_doc_data][locking][mutual-exclusion]")
{
  // Two documents, each with several transactions, processed by enough worker threads that
  // multiple on_type(txn) calls (across BOTH documents, all sharing the SAME run_data()) are
  // provably in flight at once, all contending for run_data()'s own mutex.
  constexpr int  per_doc_txns   = 4;
  constexpr int  num_docs       = 2;
  constexpr auto lock_hold_time = std::chrono::milliseconds(20); // delay INSIDE fsp::lock() -- see on_type(txn)'s own comment
  temp_dir_guard dir;
  const auto     doc_path_a = dir.write("doc_a.xml", make_doc(per_doc_txns));
  const auto     doc_path_b = dir.write("doc_b.xml", make_doc(per_doc_txns));

  auto                    observed = std::make_shared<observed_state>();
  run_doc_data_test_hooks hooks(observed, lock_hold_time);
  auto                    cfg = make_cfg("test-mutual-exclusion", 4); // 4 worker threads -- more than enough to contend

  const auto start   = std::chrono::steady_clock::now();
  auto [p, res]      = fsp::importer::exec(cfg, std::vector<std::string>{doc_path_a, doc_path_b}, xsd_path(), hooks);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(res.has_value());
  CHECK(p->ds_dscr()[0].status().ok());
  CHECK(p->ds_dscr()[1].status().ok());

  // per_doc_txns * num_docs on_type(txn) calls each hold run_data()'s own lock for lock_hold_time
  // -- if fsp::lock() genuinely serializes them (as it must: locked_root's std::lock_guard makes
  // overlapping critical sections on the SAME mutex impossible), the total time spent INSIDE that
  // one shared critical section is at least (per_doc_txns * num_docs) * lock_hold_time, no matter
  // how many worker threads run concurrently otherwise -- a second thread reaching fsp::lock()
  // while the first still holds it MUST block until the first's guard is destroyed. Using 70% of
  // the fully-serialized minimum as the threshold (not 100%) absorbs scheduling jitter/measurement
  // noise while still being far above what any overlapping-critical-sections bug could produce
  // (which would finish in roughly ONE lock_hold_time, not (per_doc_txns*num_docs) of them).
  const auto fully_serialized_minimum = lock_hold_time * (per_doc_txns * num_docs);
  CHECK(elapsed >= fully_serialized_minimum * 7 / 10);

  // And once each successive thread does get in, it sees the FULL, correctly-accumulated result
  // of every earlier holder's write -- not a torn/partial one -- confirmed by run_data()'s own
  // total_segments_seen (copied out in on_run_end(), the last hook still holding a valid
  // run_data(), since it's destroyed right after on_run_end() returns -- see
  // pipeline::process_files()) landing on EXACTLY the expected total, never less: a single lost
  // update anywhere (the exact failure mode unsynchronized concurrent += would produce) would
  // undershoot this.
  CHECK(observed->run_end_seen);
  CHECK(observed->total_segments_seen_at_run_end == per_doc_txns * num_docs);
}

// --- Test 3: fsp::lock() genuinely serializes concurrent writers -- N transactions processed by
// several worker threads at once, each incrementing doc_data(doc_ndx)->actual_count under
// fsp::lock() with an artificial delay INSIDE the critical section. Without real mutual
// exclusion, concurrent non-atomic ++ on the same std::size_t would lose updates; with it, the
// final count is exactly num_txns every time -- observable via on_doc_sem_check()'s own verdict
// (actual_count == declared_count), which is the pipeline's authoritative semantic_status(). -----
TEST_CASE("run_doc_data: fsp::lock() serializes concurrent on_type() writers -- no lost updates", "[pipeline][run_doc_data][locking]")
{
  constexpr int  num_txns = 8;
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", make_doc(num_txns));

  // seg_delay inside the lock + enough worker threads to genuinely overlap on this document's
  // several transaction segments -- forces real contention on the same doc_data() instance's
  // mutex, not just sequential access that happens to never race.
  auto                    observed = std::make_shared<observed_state>();
  run_doc_data_test_hooks hooks(observed, std::chrono::milliseconds(15));
  auto                    cfg = make_cfg("test-lock-serializes", 4);
  auto [p, res]               = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& status = p->ds_dscr()[0].status();
  // on_doc_sem_check() itself compared actual_count == declared_count (see the hook's own body
  // above) and returned that as the document's semantic verdict -- semantic_status() == valid
  // here is the pipeline-level proof that fsp::lock() prevented every lost update: had even one
  // of num_txns concurrent on_type(txn) increments been lost to a race, actual_count would have
  // undershot declared_count (num_txns) and this would be three_state::invalid instead. The exact
  // counts, captured under the SAME lock that produced the verdict (see on_doc_sem_check()'s own
  // body), confirm it directly rather than only through the aggregate status.
  CHECK(observed->actual_count_at_sem_check == num_txns);
  CHECK(observed->declared_count_seen == num_txns);
  CHECK(status.semantic_status() == fsp::three_state::valid);
  CHECK(status.ok());
  CHECK(p->get_results().size() == num_txns + 1); // header + every transaction
}
// NOLINTEND(readability-magic-numbers)
