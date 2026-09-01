// test_pipeline_stages.cpp
//
// Pipeline-level tests for the C(cutter)/V(validator)/P(processor) three-way race the
// pipeline_hooks rework (on_doc_cutting_end/on_doc_sem_check/on_doc_close, doc_status_t,
// make_doc_data()/cb_data_root) is meant to resolve deterministically -- see fsp's own
// pipeline_hooks.hpp doc comments for the full design.
//
// Reuses fsp::work (src/test/work.hpp, the same pacs8 schema pacs8.cpp/pacs8-cb.cpp/t_refl.cpp
// already exercise) and xsd/pacs.008.xsd + xml-data/pacs8-2.xml/pacs8-2-fail.xml as fixtures --
// there is no lighter-weight existing importer/pipeline test fixture in the repo, and building a
// whole second reflected schema+XSD pair just for these four tests would duplicate that
// infrastructure for no real benefit (point made explicit in the task brief).
//
// Determinism note: tests 1-3 exercise genuine cross-thread races (which of C/V/P reports last).
// To make the outcome deterministic rather than "usually correct", every scenario below forces
// exactly ONE worker thread pair (num_of_workers=2 -- one thread ends up on C/P duty, the other
// free to pick up V) PLUS an artificial delay inside on_seg_sem_check() (P's own per-segment
// hook) so that P provably has NOT finished processing the document's segments before C/V(the
// thread not currently busy cutting) has had time to run to completion first. Where the race
// direction itself needs to be reversed (test 2: C fails before P would even get segments to
// process), the delay is unnecessary -- C's failure is synchronous with respect to P even without
// one, since P can only dequeue segments C has already pushed.
#include "doc_cutter.hpp"
#include "importer.hpp"
#include "typed_semantic_check.hpp"
#include "work.hpp" // IWYU pragma: keep -- ^^fsp::work needs the actual schema classes
#include "xerces_mgr.hpp"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <logger/logger_config.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{
  namespace fs = std::filesystem;

  // --- shared test fixtures, mirroring test_exporter.cpp's/test_xml_writer.cpp's own style ---

  class temp_dir_guard
  {
  public:
    temp_dir_guard()
    : dir_(fs::temp_directory_path() / ("fsp_pipeline_stages_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter_++)))
    { fs::create_directory(dir_); }
    ~temp_dir_guard()
    {
      std::error_code ec;
      fs::remove_all(dir_, ec);
    }
    temp_dir_guard(const temp_dir_guard&)            = delete;
    temp_dir_guard& operator=(const temp_dir_guard&) = delete;
    temp_dir_guard(temp_dir_guard&&)                 = delete;
    temp_dir_guard& operator=(temp_dir_guard&&)      = delete;
    // Writes content to <dir>/name, returns the full path (string, as pipeline::process_files()
    // wants for xml_paths).
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

  // Resolves xsd/pacs.008.xsd against the repository root -- CMAKE_SOURCE_DIR is injected via a
  // compile definition (see CMakeLists.txt's unit_tests target) so this works regardless of the
  // build directory ctest/the raw binary is invoked from.
  std::string xsd_path() { return std::string(FSP_TEST_SOURCE_DIR) + "/xsd/pacs.008.xsd"; }

  // --- minimal well-formed-but-schema-invalid pacs8 fixture (schema/validity violation: swaps
  // the required InstdAgt for a duplicate InstgAgt inside GrpHdr, same technique as the existing
  // xml-data/pacs8-2-fail.xml fixture) -- kept inline (not a second data file) so every test
  // scenario below can freely tweak transaction counts without touching shared fixture files. ---
  constexpr auto* k_valid_hdr = R"(<x:GrpHdr>
      <x:MsgId>MSG1</x:MsgId>
      <CreDtTm>2026-05-14T07:41:50.161Z</CreDtTm>
      <NbOfTxs>1</NbOfTxs>
      <TtlIntrBkSttlmAmt Ccy="EUR">1.00</TtlIntrBkSttlmAmt>
      <IntrBkSttlmDt>2026-05-14</IntrBkSttlmDt>
      <SttlmInf><SttlmMtd>CLRG</SttlmMtd></SttlmInf>
      <PmtTpInf><SvcLvl><Cd>SEPA</Cd></SvcLvl><CtgyPurp><Cd>FCIN</Cd></CtgyPurp></PmtTpInf>
      <InstgAgt><FinInstnId><BICFI>FHQIBWWK</BICFI></FinInstnId></InstgAgt>
      <InstdAgt><FinInstnId><BICFI>KWNRKEQS</BICFI></FinInstnId></InstdAgt>
    </x:GrpHdr>)";

  // Same header but schema-INVALID: InstdAgt duplicated as a second InstgAgt (mirrors
  // xml-data/pacs8-2-fail.xml) -- well-formed XML, fails XSD validation.
  constexpr auto* k_schema_invalid_hdr = R"(<x:GrpHdr>
      <x:MsgId>MSG1</x:MsgId>
      <CreDtTm>2026-05-14T07:41:50.161Z</CreDtTm>
      <NbOfTxs>1</NbOfTxs>
      <TtlIntrBkSttlmAmt Ccy="EUR">1.00</TtlIntrBkSttlmAmt>
      <IntrBkSttlmDt>2026-05-14</IntrBkSttlmDt>
      <SttlmInf><SttlmMtd>CLRG</SttlmMtd></SttlmInf>
      <PmtTpInf><SvcLvl><Cd>SEPA</Cd></SvcLvl><CtgyPurp><Cd>FCIN</Cd></CtgyPurp></PmtTpInf>
      <InstgAgt><FinInstnId><BICFI>FHQIBWWK</BICFI></FinInstnId></InstgAgt>
      <InstgAgt><FinInstnId><BICFI>KWNRKEQS</BICFI></FinInstnId></InstgAgt>
    </x:GrpHdr>)";

  constexpr auto* k_valid_txn = R"(<CdtTrfTxInf>
      <PmtId><EndToEndId>E2E1</EndToEndId><TxId>TXN1</TxId></PmtId>
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
    </CdtTrfTxInf>)";

  std::string wrap_document(std::string_view hdr, std::string_view txns)
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

  std::string well_formed_valid_doc() { return wrap_document(k_valid_hdr, k_valid_txn); }
  std::string well_formed_schema_invalid_doc() { return wrap_document(k_schema_invalid_hdr, k_valid_txn); }

  // --- multi-transaction fixture, for on_doc_stored()/header-priority/shard tests below, which
  // need enough segments per document to exercise multiple flush batches and genuine cross-shard/
  // cross-thread traffic -- a single-transaction document (well_formed_valid_doc() above) can
  // never do that. NbOfTxs in the header must match num_txns exactly, or schema validation fails. ---
  std::string multi_txn_hdr(int num_txns)
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

  std::string multi_txn_one(int n)
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

  // Document with num_txns transactions (see multi_txn_hdr()/multi_txn_one() above) -- enough
  // P-role segments for multiple flush batches and genuine cross-shard/cross-thread traffic.
  std::string multi_txn_doc(int num_txns)
  {
    std::string txns;
    for (int i = 0; i < num_txns; ++i) txns += multi_txn_one(i);
    return wrap_document(multi_txn_hdr(num_txns), txns);
  }
  // Not well-formed at all: an unclosed/mismatched tag inside the header -- Handler::fatalError()
  // territory (well-formedness), not Handler::error() (schema/validity).
  std::string ill_formed_doc()
  {
    return wrap_document("<x:GrpHdr><x:MsgId>MSG1</x:MsgId><CreDtTm>2026-05-14T07:41:50.161Z</CreDtTm><NbOfTxs>1</NbOfTxs></x:BadClose>",
                         k_valid_txn);
  }

  // --- test-only pipeline_hooks: tracks call order/timing/verdicts with atomics (this class is
  // cloned once per worker thread -- see pipeline_hooks.hpp's own doc comment -- so every field
  // that must be visible ACROSS clones needs to live behind a shared_ptr, not directly in *this).
  // ---
  struct shared_state
  {
    std::atomic<int>          seg_sem_check_calls{0};
    std::atomic<int>          doc_sem_check_calls{0};
    std::atomic<int>          doc_close_calls{0};
    std::atomic<bool>         doc_sem_check_seen_all_segments_done{false};
    std::atomic<bool>         doc_close_seen_syntax_ok{false};
    std::atomic<bool>         doc_close_seen_validation_ok{false};
    std::atomic<bool>         doc_close_seen_semantic_ok{false};
    std::atomic<int>          segments_ever_seen{0};        // on_seg_sem_check() call count, cross-clone
    std::chrono::milliseconds seg_delay{0};                 // artificial per-segment delay, see class doc comment
    bool                      doc_sem_check_verdict = true; // what on_doc_sem_check() should return
    // --- on_type()'s own new raw_msg/dscr parameters (see typed_semantic_check.hpp's own class
    // comment) -- captured by on_type(hdr,...)/on_type(txn,...) below, one shared_ptr<string> per
    // captured raw_msg (not a plain std::string, so a std::atomic<std::string> isn't needed) plus
    // one atomic snapshot of dscr.agent_id() per hook. ---
    std::mutex                raw_msg_mutex; // guards hdr_raw_msg/txn_raw_msg (plain strings, not atomics -- string isn't lock-free)
    fsp::str_t                hdr_raw_msg;   // last on_type(hdr,...)'s own raw_msg, verbatim
    fsp::str_t                txn_raw_msg;   // last on_type(txn,...)'s own raw_msg, verbatim
    std::atomic<bool>         hdr_agent_id_has_value{false};
    std::atomic<std::int16_t> hdr_agent_id_value{0};
    std::atomic<bool>         txn_agent_id_has_value{false};
    std::atomic<std::int16_t> txn_agent_id_value{0};
    // What on_type(hdr,...)/on_type(txn,...) below should return -- true (the default) for every
    // existing scenario in this file; a HE/TE-class TEST_CASE flips one of these to false to force
    // that specific segment kind's own semantic check to fail.
    std::atomic<bool> hdr_verdict{true};
    std::atomic<bool> txn_verdict{true};
    // on_remove_stored_data_safe()'s own (out_doc_id, no_headers) arguments, captured the same way
    // doc_close_err_message is above -- checked by the error_class-related TEST_CASEs below.
    std::atomic<int>  remove_stored_data_calls{0};
    std::atomic<std::uint64_t> remove_stored_data_out_doc_id{0};
    std::atomic<bool>          remove_stored_data_no_headers{false};
    std::mutex                doc_close_err_mutex;   // guards doc_close_err_message (plain string, not lock-free)
    fsp::str_t                doc_close_err_message; // last on_doc_close()'s own err.message(), verbatim
    std::atomic<int>          doc_close_segments_stored{-1}; // last on_doc_close()'s own segments_stored argument
    // --- on_doc_stored()/on_doc_close() ordering + per-document accounting (see the on_doc_stored
    // TEST_CASEs below) -- a single, monotonically increasing global sequence counter is stamped
    // into per-doc_ndx slots by both hooks, so a test can assert stored's own stamp is strictly
    // less than close's own stamp for EVERY document, not just "both eventually fired". Sized to
    // the actual document count by each TEST_CASE that uses these (resize() before exec()). ---
    std::atomic<int>              global_seq{0};
    std::vector<std::atomic<int>> doc_stored_seq;      // -1 until on_doc_stored() fires for that doc_ndx
    std::vector<std::atomic<int>> doc_close_seq;       // -1 until on_doc_close() fires for that doc_ndx
    std::atomic<int>              doc_stored_calls{0}; // total on_doc_stored() calls, cross-clone, cross-document
    // Set if either hook below ever observes a violation of its own contract (fired twice for the
    // same doc_ndx, or doc_ndx out of range) -- checked with REQUIRE() from the MAIN test thread
    // after exec() returns, never from inside the hook itself: Catch2's assertion machinery is not
    // documented as safe to call concurrently from multiple worker threads, unlike the plain
    // atomics/mutexes shared_state already uses everywhere else in this file.
    std::atomic<bool> doc_stored_contract_violated{false};
    std::atomic<bool> doc_close_seq_contract_violated{false};
    // Resizes doc_stored_seq/doc_close_seq to doc_count slots, all seeded to -1 ("not fired yet").
    // std::atomic isn't movable/copyable, so this can't just be vector::resize() with a fill value
    // -- rebuild from scratch instead. Call once, before exec(), from the main thread only.
    void size_doc_seqs(std::size_t doc_count)
    {
      doc_stored_seq = std::vector<std::atomic<int>>(doc_count);
      doc_close_seq  = std::vector<std::atomic<int>>(doc_count);
      for (auto& s : doc_stored_seq) s.store(-1, std::memory_order_relaxed);
      for (auto& s : doc_close_seq) s.store(-1, std::memory_order_relaxed);
    }
  };

  // A tiny doc-level doc_data_root, used only to prove the DocData template argument/
  // pipeline::doc_data() wiring reaches on_doc_sem_check() -- content itself isn't exercised by
  // these tests beyond existing. reset() clears touched so a recycled instance starts fresh for
  // the next document that reuses it (see fsp::doc_data_root::reset()'s own doc comment).
  struct test_doc_data : fsp::doc_data_root
  {
    std::atomic<int> touched{0};
    void             reset() override
    {
      touched.store(0, std::memory_order_relaxed);
      fsp::doc_data_root::reset();
    }
  };

  class stage_test_hooks
  : public fsp::typed_semantic_check<stage_test_hooks, ^^fsp::work, fsp::seg_schema, fsp::run_data_root, test_doc_data>
  {
  public:
    // resolved_agent_id: what get_doc_agent_id() below should resolve every document's path to --
    // std::nullopt (the default) exercises pipeline_hooks::get_doc_agent_id()'s own default no-op
    // body instead (see its own doc comment), same as a hook that never overrides it at all.
    explicit stage_test_hooks(std::shared_ptr<shared_state> state, std::optional<std::int16_t> resolved_agent_id = std::nullopt)
    : state_(std::move(state))
    , resolved_agent_id_(resolved_agent_id)
    {
    }
    [[nodiscard]] bool on_type(const fsp::work::pacs8_hdr& /*hdr*/,
                               std::string_view     raw_msg,
                               const fsp::doc_dscr& dscr,
                               fsp::segment_result& /*result*/,
                               bool /*is_first*/,
                               bool /*is_last*/) const
    {
      {
        const std::scoped_lock lock(state_->raw_msg_mutex);
        state_->hdr_raw_msg = fsp::str_t(raw_msg);
      }
      if (const auto agent_id = dscr.agent_id(); agent_id)
      {
        state_->hdr_agent_id_value.store(*agent_id, std::memory_order_relaxed);
        state_->hdr_agent_id_has_value.store(true, std::memory_order_relaxed);
      }
      on_any_segment();
      return state_->hdr_verdict.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool on_type(const fsp::work::pacs8_txn& /*txn*/,
                               std::string_view     raw_msg,
                               const fsp::doc_dscr& dscr,
                               fsp::segment_result& /*result*/,
                               bool /*is_first*/,
                               bool /*is_last*/) const
    {
      {
        const std::scoped_lock lock(state_->raw_msg_mutex);
        state_->txn_raw_msg = fsp::str_t(raw_msg);
      }
      if (const auto agent_id = dscr.agent_id(); agent_id)
      {
        state_->txn_agent_id_value.store(*agent_id, std::memory_order_relaxed);
        state_->txn_agent_id_has_value.store(true, std::memory_order_relaxed);
      }
      on_any_segment();
      return state_->txn_verdict.load(std::memory_order_relaxed);
    }
  protected:
    bool on_doc_sem_check(std::size_t doc_ndx) override
    {
      state_->doc_sem_check_calls.fetch_add(1, std::memory_order_relaxed);
      // Every segment's on_seg_sem_check() must have already run by the time doc-level semantics
      // are checked (doc_counters' "all segments processed" completion condition, independent of
      // V) -- see doc_counters.hpp.
      if (state_->segments_ever_seen.load(std::memory_order_relaxed) > 0)
        state_->doc_sem_check_seen_all_segments_done.store(true, std::memory_order_relaxed);
      doc_data(doc_ndx).touched.fetch_add(1, std::memory_order_relaxed);
      return state_->doc_sem_check_verdict;
    }
    // Stamps doc_ndx's own slot in doc_stored_seq with the next tick of the shared global_seq
    // counter -- see shared_state's own doc comment on why this, not a plain bool, is what lets a
    // test assert ORDERING against on_doc_close()'s own stamp below, not just "both fired".
    [[nodiscard]] fsp::e_void on_doc_stored(std::size_t doc_ndx, const fsp::doc_dscr& /*dscr*/) override
    {
      state_->doc_stored_calls.fetch_add(1, std::memory_order_relaxed);
      const int seq = state_->global_seq.fetch_add(1, std::memory_order_relaxed);
      if (doc_ndx >= state_->doc_stored_seq.size()) // caller forgot shared_state::size_doc_seqs()
        state_->doc_stored_contract_violated.store(true, std::memory_order_relaxed);
      else if (const int prev = state_->doc_stored_seq[doc_ndx].exchange(seq, std::memory_order_relaxed); prev != -1)
        state_->doc_stored_contract_violated.store(true, std::memory_order_relaxed); // fired more than once for doc_ndx
      return {};
    }
    [[nodiscard]] bool on_doc_close(std::size_t              doc_ndx,
                                    const fsp::doc_status_t& verdict,
                                    const fsp::error_info&   err,
                                    const fsp::doc_dscr& /*dscr*/,
                                    std::size_t              segments_stored) override
    {
      state_->doc_close_calls.fetch_add(1, std::memory_order_relaxed);
      state_->doc_close_segments_stored.store(static_cast<int>(segments_stored), std::memory_order_relaxed);
      if (doc_ndx < state_->doc_close_seq.size()) // only sized by TEST_CASEs that actually check ordering
      {
        const int seq = state_->global_seq.fetch_add(1, std::memory_order_relaxed);
        if (const int prev = state_->doc_close_seq[doc_ndx].exchange(seq, std::memory_order_relaxed); prev != -1)
          state_->doc_close_seq_contract_violated.store(true, std::memory_order_relaxed); // fired more than once for doc_ndx
      }
      state_->doc_close_seen_syntax_ok.store(verdict.syntax_status() == fsp::three_state::valid, std::memory_order_relaxed);
      state_->doc_close_seen_validation_ok.store(verdict.valid_status() == fsp::three_state::valid, std::memory_order_relaxed);
      state_->doc_close_seen_semantic_ok.store(verdict.semantic_status() == fsp::three_state::valid, std::memory_order_relaxed);
      {
        const std::scoped_lock lock(state_->doc_close_err_mutex);
        state_->doc_close_err_message = fsp::str_t(err.message());
      }
      return verdict.ok();
    }
    // Override point exercised directly (not via a real BIC4/dictionary lookup -- see
    // ach_hook::get_doc_agent_id() for that concrete use case) -- proves pipeline::add_documents()
    // calls this hook and stores its result into doc_dscr::agent_id() BEFORE any worker thread
    // starts (same call site/ordering as get_doc_id(), see pipeline_hooks.hpp's own doc comment).
    [[nodiscard]] std::optional<std::int16_t> get_doc_agent_id(fsp::cstr_t /*path*/) override { return resolved_agent_id_; }
    // Records the (out_doc_id, no_headers) pipeline::report_error_class() resolved -- see
    // pipeline_hooks.hpp's own doc comment on on_remove_stored_data_safe() for the full contract.
    // A single-document TEST_CASE only ever expects this to fire once (see doc_status_t::
    // mark_error()'s own "first error class only" doc comment), so no per-document indexing here.
    [[nodiscard]] fsp::e_void on_remove_stored_data(std::uint64_t out_doc_id, bool no_headers) override
    {
      state_->remove_stored_data_calls.fetch_add(1, std::memory_order_relaxed);
      state_->remove_stored_data_out_doc_id.store(out_doc_id, std::memory_order_relaxed);
      state_->remove_stored_data_no_headers.store(no_headers, std::memory_order_relaxed);
      return {};
    }
  private:
    // Common tail for both on_type() overloads above -- const because on_type() itself is const
    // (typed_semantic_check's own contract), but state_ is a shared_ptr to shared, atomically
    // synchronized state, so mutating *state_ through a const stage_test_hooks& is safe.
    void on_any_segment() const
    {
      state_->seg_sem_check_calls.fetch_add(1, std::memory_order_relaxed);
      state_->segments_ever_seen.fetch_add(1, std::memory_order_relaxed);
      if (state_->seg_delay.count() > 0) std::this_thread::sleep_for(state_->seg_delay);
    }
    std::shared_ptr<shared_state> state_;
    std::optional<std::int16_t>   resolved_agent_id_; // what get_doc_agent_id() resolves every document's path to
  };

  // Minimal hooks class that does NOT override get_doc_agent_id() at all -- unlike stage_test_hooks
  // above (which always overrides it, even to explicitly return std::nullopt), this is the only way
  // to exercise pipeline_hooks::get_doc_agent_id()'s own, truly-unoverridden default body (see its
  // own doc comment in pipeline_hooks.hpp on why that default is 0, not std::nullopt).
  class no_agent_override_hooks : public fsp::typed_semantic_check<no_agent_override_hooks, ^^fsp::work>
  {
  public:
    [[nodiscard]] bool on_type(const fsp::work::pacs8_hdr& /*hdr*/,
                               std::string_view /*raw_msg*/,
                               const fsp::doc_dscr& /*dscr*/,
                               fsp::segment_result& /*result*/,
                               bool /*is_first*/,
                               bool /*is_last*/) const
    { return true; }
    [[nodiscard]] bool on_type(const fsp::work::pacs8_txn& /*txn*/,
                               std::string_view /*raw_msg*/,
                               const fsp::doc_dscr& /*dscr*/,
                               fsp::segment_result& /*result*/,
                               bool /*is_first*/,
                               bool /*is_last*/) const
    { return true; }
  };

  fsp::importer_config make_cfg(std::string_view app_name, std::size_t num_of_workers)
  {
    return fsp::importer_config{.targets        = fsp::proc_data_of<^^fsp::work>(),
                                .num_of_workers = num_of_workers,
                                .log_config     = silent_log_cfg(app_name),
                                .program_name   = std::string(app_name)};
  }

  // Extended knob set for the on_doc_stored()/header-priority/shard TEST_CASEs below, which need
  // to force small flush batches (ok_block_flush_size) and specific shard counts
  // (pool_shard_count) -- see each TEST_CASE's own comment for why. The header-priority mechanism
  // itself is no longer a runtime knob here (see fsp::work::pacs8_hdr, now permanently
  // hdr_seg_schema-derived) -- every TEST_CASE using fsp::work already exercises it.
  fsp::importer_config make_cfg_ex(std::string_view app_name,
                                   std::size_t      num_of_workers,
                                   std::size_t      pool_shard_count,
                                   std::size_t      ok_block_flush_size)
  {
    return fsp::importer_config{.targets              = fsp::proc_data_of<^^fsp::work>(),
                                .num_of_workers       = num_of_workers,
                                .pool_shard_count     = pool_shard_count,
                                .ok_block_flush_size  = ok_block_flush_size,
                                .nak_block_flush_size = ok_block_flush_size,
                                .log_config           = silent_log_cfg(app_name),
                                .program_name         = std::string(app_name)};
  }
} // namespace

// NOLINTBEGIN(readability-magic-numbers) -- arbitrary test fixture literals (delays, counts)

// --- Scenario 1: V finishes (fails) before P is done -- segments must be discarded ------------
TEST_CASE("pipeline: V failing before P finishes discards the document's segments", "[pipeline][stages][V-before-P]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_schema_invalid_doc());

  auto state       = std::make_shared<shared_state>();
  state->seg_delay = std::chrono::milliseconds(80); // gives V (running in parallel) time to fail first
  stage_test_hooks hooks(state);

  // num_of_workers=2: one thread ends up cutting+processing (C then P), the other is free to run
  // V concurrently -- with the segment delay above, V (a single synchronous XSD pass, no
  // per-segment hook to slow it down) reliably reports failure before P's own delayed
  // on_seg_sem_check() calls return.
  auto cfg      = make_cfg("test-v-before-p", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  CHECK(res->total_docs() == 1);
  const auto& ds_dscr = p->ds_dscr();
  const auto& status  = ds_dscr[0].status();
  CHECK(status.syntax_status() == fsp::three_state::invalid); // schema failure drags syntax invalid too (point 14)
  CHECK(status.valid_status() == fsp::three_state::invalid);
  // semantic_ is invalid too, NOT because on_doc_sem_check() itself ran and found something (it
  // does not even get called - already_rejected is true here) but because
  // maybe_finish_seg_processing() now reports semantic_ok=false whenever already_rejected is true,
  // for the SAME reason error_class::he needs this (see that method's own doc comment in
  // pipeline.cpp): set_semantic_result(false) is what drives doc_status_t::done_ to its
  // k_done_threshold short-circuit for THIS document too, exactly like syntax_/valid_ already did
  // on their own two lines up - status() (the aggregate verdict) was already invalid before this
  // change; only semantic_status() specifically (a strictly narrower question) flipped.
  CHECK(status.semantic_status() == fsp::three_state::invalid);
  // A genuine XSD schema violation (duplicated InstgAgt, see well_formed_schema_invalid_doc()) is
  // error_class::ve, not error_class::se -- the document IS well-formed XML, just schema-invalid.
  CHECK(ds_dscr[0].has_error(fsp::error_class::ve));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::se));
  CHECK(state->remove_stored_data_calls.load() == 1);
  CHECK_FALSE(state->remove_stored_data_no_headers.load());

  // The document's segments were discarded (xml_worker::process_one()'s own "document invalid,
  // skip" path) -- total_segments_ok()/total_segments_error() only count a syntactically correct
  // document's segments (see doc_set_counter::syntactically_correct()), so a rejected document
  // contributes 0 to both regardless of how many segments P processed (or started processing)
  // before the rejection became known.
  CHECK(res->total_segments_ok(ds_dscr) == 0);
  CHECK(res->total_segments_error(ds_dscr) == 0);

  // on_doc_close() must still fire exactly once, reflecting the failed verdict.
  CHECK(state->doc_close_calls.load() == 1);
  CHECK_FALSE(state->doc_close_seen_syntax_ok.load());
  CHECK_FALSE(state->doc_close_seen_validation_ok.load());
}

// --- Scenario 2: C fails (ill-formed XML) before P has anything to process ---------------------
TEST_CASE("pipeline: C failing on ill-formed XML discards segments and still closes the doc once", "[pipeline][stages][C-fails]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", ill_formed_doc());

  auto state = std::make_shared<shared_state>();
  // No artificial delay needed here: P can only dequeue segments C has already cut, and C fails
  // immediately (well-formedness violation inside GrpHdr, before any segment-closing tag is
  // even reached) -- see Handler::endElement()/fatalError(). V, however, DOES get a chance to run
  // (it reads straight from the mmap'd string_view, independent of C -- point 4 of the design
  // discussion) and is exercised here too: it must not crash and must agree the document is
  // invalid, whichever of C/V ends up winning the race to report first.
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-c-fails", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  const auto& status  = ds_dscr[0].status();
  CHECK(status.syntax_status() == fsp::three_state::invalid);
  CHECK(status.valid_status() == fsp::three_state::invalid);
  CHECK(ds_dscr[0].failed());
  // error_info::code() must reflect a well-formedness problem (parse_failed), not a schema one
  // (xsd_validation_failed) -- point 15/16 of the design discussion: Handler::fatalError() (well-
  // formedness) vs. error() (schema) is now distinguished via sax_error_source, independent of
  // which of C/V happens to be the one that reports it first.
  const auto code = ds_dscr[0].error().code();
  CHECK((code == fsp::processor_error::parse_failed || code == fsp::processor_error::xsd_validation_failed));
  // error_class::se, from whichever of C/V actually reports this well-formedness problem first --
  // see the SE/VE branches in pipeline_worker.cpp's do_cut()/do_validate() for why this is always
  // se here, never ve (both branches classify a well-formedness failure as se, an XSD schema
  // violation as ve -- this fixture is ill-formed, not merely schema-invalid).
  CHECK(ds_dscr[0].has_error(fsp::error_class::se));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ve));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ua));

  CHECK(res->total_segments_ok(ds_dscr) == 0);
  // on_doc_close() fires exactly once even though the document never produced a single
  // successfully-cut segment.
  CHECK(state->doc_close_calls.load() == 1);
  CHECK_FALSE(state->doc_close_seen_syntax_ok.load());
  // on_remove_stored_data_safe() fired exactly once, no_headers=false.
  CHECK(state->remove_stored_data_calls.load() == 1);
  CHECK_FALSE(state->remove_stored_data_no_headers.load());
}

// --- Scenario 3: C+P agree the doc-level semantics are wrong while V is still delayed ----------
TEST_CASE("pipeline: on_doc_sem_check's false verdict survives to on_doc_close even if V finishes later", "[pipeline][stages][sem-then-V]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc()); // syntactically+schema OK

  auto state                   = std::make_shared<shared_state>();
  state->doc_sem_check_verdict = false;                        // C+P (doc-level) declare the document semantically wrong
  state->seg_delay             = std::chrono::milliseconds(0); // P itself doesn't need to be slow here
  stage_test_hooks hooks(state);

  // A single worker thread forces V to run strictly AFTER C+P have already finished (V only gets
  // picked once this one thread has drained both c_queue_ and every ready segment -- see
  // pipeline_worker::operator()'s own C > V > P priority order) -- the most deterministic way to
  // guarantee on_doc_sem_check() (fed into doc_status_t::set_semantic()) completes before V's own
  // set_valid() call, without relying on a timing coincidence.
  auto cfg      = make_cfg("test-sem-then-v", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  const auto& status  = ds_dscr[0].status();
  CHECK(status.syntax_status() == fsp::three_state::valid);
  CHECK(status.valid_status() == fsp::three_state::valid);
  CHECK(status.semantic_status() == fsp::three_state::invalid); // on_doc_sem_check()'s false verdict must survive to doc_status_t

  CHECK(state->doc_sem_check_calls.load() == 1);
  CHECK(state->doc_sem_check_seen_all_segments_done.load()); // ran only once all segments were done
  CHECK(state->doc_close_calls.load() == 1);                 // fires exactly once
  CHECK(state->doc_close_seen_syntax_ok.load());
  CHECK(state->doc_close_seen_validation_ok.load());
  CHECK_FALSE(state->doc_close_seen_semantic_ok.load()); // and carries on_doc_sem_check()'s captured verdict
}

// --- Scenario 4: happy path -- on_doc_close fires exactly once, only once everything is known --
TEST_CASE("pipeline: happy path calls on_doc_close exactly once, after syntax+validation+semantics are all known",
          "[pipeline][stages][happy-path]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state       = std::make_shared<shared_state>();
  state->seg_delay = std::chrono::milliseconds(30); // makes the race window observable even here
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-happy-path", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  const auto& status  = ds_dscr[0].status();
  CHECK(status.ok());
  CHECK(status.syntax_status() == fsp::three_state::valid);
  CHECK(status.valid_status() == fsp::three_state::valid);
  CHECK(status.semantic_status() == fsp::three_state::valid);

  CHECK(state->doc_sem_check_calls.load() == 1);
  CHECK(state->doc_sem_check_seen_all_segments_done.load());
  CHECK(state->doc_close_calls.load() == 1); // never called before, never called twice
  CHECK(state->doc_close_seen_syntax_ok.load());
  CHECK(state->doc_close_seen_validation_ok.load());
  CHECK(state->doc_close_seen_semantic_ok.load());

  // Both segments (header + one transaction) were actually processed, and none discarded.
  CHECK(res->total_segments_ok(ds_dscr) == 2);
  CHECK(res->total_segments_error(ds_dscr) == 0);
}

// --- Scenario 4b: direct, pipeline-free unit check that fsp::pipeline_hooks::get_doc_agent_id()'s
// own base-class body -- called straight on a plain fsp::no_op_hooks instance, no exec()/cutting/
// validation involved at all -- returns 0, not std::nullopt (see pipeline_hooks.hpp's own doc
// comment on why 0, the fail-closed default, replaced the old std::nullopt one). Scenario 5 below
// is the integration-level counterpart (proves the SAME default rejects a document end-to-end via
// the real pipeline) -- this one isolates just the virtual call itself. ---------------------------
TEST_CASE("pipeline_hooks: get_doc_agent_id()'s own base-class default body returns 0", "[pipeline_hooks][agent-id]")
{
  fsp::no_op_hooks hooks;
  const auto       result = hooks.get_doc_agent_id("irrelevant/path.xml");
  REQUIRE(result.has_value());
  CHECK(*result == 0);
}

// --- Scenario 5: get_doc_agent_id()'s truly-unoverridden default body returns 0 (fsp-core's own
// "unresolved agent" convention), NOT std::nullopt -- a hook that never touches this override point
// at all therefore has every document rejected before any cut/validate work, as a fail-closed
// default (see pipeline_hooks.hpp's own doc comment on why this default changed from std::nullopt).
// Needs no_agent_override_hooks, not stage_test_hooks -- see its own doc comment on why. -----------
TEST_CASE("pipeline: get_doc_agent_id()'s truly-unoverridden default (0) rejects the document", "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  no_agent_override_hooks hooks;

  auto cfg      = make_cfg("test-agent-id-unoverridden-default", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  REQUIRE(ds_dscr[0].agent_id().has_value());
  CHECK(*ds_dscr[0].agent_id() == 0);
  CHECK_FALSE(ds_dscr[0].status().ok()); // rejected on the agent_id()==0 convention, same as Scenario 6b below
}

// --- Scenario 5b: a hook that DOES override get_doc_agent_id(), but explicitly returns
// std::nullopt (as opposed to never overriding it at all, see Scenario 5 above) -- doc_dscr::
// agent_id() stays genuinely unset, and the agent_id()==0 rejection does NOT fire for it (unset is
// not the same as 0, see doc_dscr::agent_id()'s own doc comment) -- the document is processed
// normally. -------------------------------------------------------------------------------------
TEST_CASE("pipeline: get_doc_agent_id() explicitly returning std::nullopt leaves doc_dscr::agent_id() unset, "
          "document processed normally",
          "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state = std::make_shared<shared_state>();
  // No resolved_agent_id passed -- stage_test_hooks::get_doc_agent_id() still overrides the hook,
  // explicitly returning std::nullopt (its own default constructor argument) -- distinct from
  // Scenario 5 above, which never overrides the hook at all.
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-agent-id-explicit-nullopt", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  CHECK_FALSE(ds_dscr[0].agent_id().has_value());
  CHECK(ds_dscr[0].status().ok()); // NOT rejected -- unset agent_id() is not the same as 0

  // Both on_type() overloads observed the same unset state through their own dscr parameter.
  CHECK_FALSE(state->hdr_agent_id_has_value.load());
  CHECK_FALSE(state->txn_agent_id_has_value.load());
}

// --- Scenario 6: get_doc_agent_id() override resolves a value -- doc_dscr::agent_id() carries it,
// on_type()'s own dscr parameter observes the SAME value, main-thread-computed strictly before any
// worker thread starts (same happens-before argument as out_doc_id(), see doc_dscr.hpp) -----------
TEST_CASE("pipeline: get_doc_agent_id()'s resolved value reaches doc_dscr::agent_id() and on_type()'s own dscr parameter",
          "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  constexpr std::int16_t resolved_id = 42;
  auto                   state       = std::make_shared<shared_state>();
  stage_test_hooks       hooks(state, resolved_id);

  auto cfg      = make_cfg("test-agent-id-resolved", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  REQUIRE(ds_dscr[0].agent_id().has_value());
  CHECK(*ds_dscr[0].agent_id() == resolved_id);

  // Both on_type() overloads (header and transaction) received the exact same resolved value
  // through their own dscr parameter -- proves the ONE hooks.get_doc_agent_id() call in
  // pipeline::add_documents() is what every later on_type() call reads back, not a per-call
  // re-resolution.
  REQUIRE(state->hdr_agent_id_has_value.load());
  CHECK(state->hdr_agent_id_value.load() == resolved_id);
  REQUIRE(state->txn_agent_id_has_value.load());
  CHECK(state->txn_agent_id_value.load() == resolved_id);
}

// --- Scenario 6b: get_doc_agent_id() resolves to 0 (a hook's own "unresolved agent" convention,
// see doc_dscr::agent_id()'s own doc comment - fsp itself never interprets the value) -- C/V both
// reject the document before doing any real cut/validate work, doc_dscr::agent_id() still carries
// the 0, and on_doc_close() still fires exactly once (verdict.ok()==false, syntax/validation both
// invalid) -- same terminal-callback guarantee as any other syntax/validation failure. ------------
TEST_CASE("pipeline: get_doc_agent_id()'s resolved 0 rejects the document before any cut/validate work", "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto             state = std::make_shared<shared_state>();
  stage_test_hooks hooks(state, static_cast<std::int16_t>(0));

  auto cfg      = make_cfg("test-agent-id-zero", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  REQUIRE(ds_dscr[0].agent_id().has_value());
  CHECK(*ds_dscr[0].agent_id() == 0);
  CHECK(ds_dscr[0].failed());
  CHECK(ds_dscr[0].rejected()); // the lock-free fast path agrees with status().status()==invalid
  CHECK(ds_dscr[0].has_error(fsp::error_class::ua));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::se));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ve));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::he));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::te));

  // Exactly one terminal callback, never a cut/validate/semantic-check attempt.
  CHECK(state->doc_close_calls.load() == 1);
  CHECK_FALSE(state->doc_close_seen_syntax_ok.load());
  CHECK_FALSE(state->doc_close_seen_validation_ok.load());
  CHECK(state->doc_sem_check_calls.load() == 0);
  CHECK(state->segments_ever_seen.load() == 0);
  CHECK(res->total_segments_ok(ds_dscr) == 0);
  CHECK(res->total_segments_error(ds_dscr) == 0);
  {
    // do_cut() always wins for such a document (do_validate() only ever bails out silently - see
    // its own doc comment on why it cannot itself call report_validation_result() here).
    const std::scoped_lock lock(state->doc_close_err_mutex);
    CHECK(state->doc_close_err_message.find("cut skipped") != fsp::str_t::npos);
  }
  // on_remove_stored_data_safe() fired exactly once, no_headers=false (UA invalidates the whole
  // document, header included -- see error_class::ua's own doc comment).
  CHECK(state->remove_stored_data_calls.load() == 1);
  CHECK(state->remove_stored_data_out_doc_id.load() == ds_dscr[0].out_doc_id());
  CHECK_FALSE(state->remove_stored_data_no_headers.load());
  // on_doc_close()'s own segments_stored parameter is 0 - no segment was ever processed at all
  // (segments_ever_seen == 0 above), so nothing could have been durably stored either. A caller
  // (e.g. ach's own remove_stored_data_for_doc() call) can use this to skip cleanup work outright
  // for exactly this shape of rejection.
  CHECK(state->doc_close_segments_stored.load() == 0);
}

// --- Scenario 6c: same as Scenario 6b, with two documents instead of one, both agent_id()==0 -
// do_validate() never rejects a document itself (it only bails out silently - see its own doc
// comment on why: it is not pipeline::assign_doc_data()'s caller, do_cut() is), so do_cut() is
// always the one that actually closes such a document, with 2+ documents same as with 1. ----------
TEST_CASE("pipeline: with 2+ documents, every agent_id()==0 document is still rejected before any cut/validate work",
          "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path_a = dir.write("doc-a.xml", well_formed_valid_doc());
  const auto     doc_path_b = dir.write("doc-b.xml", well_formed_valid_doc());

  auto             state = std::make_shared<shared_state>();
  stage_test_hooks hooks(state, static_cast<std::int16_t>(0)); // both documents resolve to agent_id 0

  auto cfg      = make_cfg("test-agent-id-zero-two-docs", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path_a, doc_path_b}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  REQUIRE(ds_dscr[0].agent_id().has_value());
  CHECK(*ds_dscr[0].agent_id() == 0);
  CHECK(ds_dscr[0].failed());
  REQUIRE(ds_dscr[1].agent_id().has_value());
  CHECK(*ds_dscr[1].agent_id() == 0);
  CHECK(ds_dscr[1].failed());

  CHECK(state->doc_close_calls.load() == 2);
  CHECK(state->doc_sem_check_calls.load() == 0);
  CHECK(state->segments_ever_seen.load() == 0); // P never ran for either document
  {
    const std::scoped_lock lock(state->doc_close_err_mutex);
    CHECK(state->doc_close_err_message.find("cut skipped") != fsp::str_t::npos);
  }
}

// --- Scenario 7: on_type()'s own raw_msg parameter carries the segment's own raw XML fragment,
// independent of (but derived from) doc_dscr's own mmap -- matches xml_segment::view() directly ---
TEST_CASE("pipeline: on_type()'s own raw_msg parameter carries the segment's raw XML fragment", "[pipeline][stages][raw-msg]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto             state = std::make_shared<shared_state>();
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-raw-msg", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const std::scoped_lock lock(state->raw_msg_mutex);
  // k_valid_hdr's/k_valid_txn's own top-level tags -- raw_msg is the exact XML subtree C cut for
  // this segment, so it must at minimum contain the fixture's own distinguishing content (MsgId
  // for the header, TxId for the transaction) verbatim.
  CHECK(state->hdr_raw_msg.find("MSG1") != fsp::str_t::npos);
  CHECK(state->hdr_raw_msg.find("GrpHdr") != fsp::str_t::npos);
  CHECK(state->txn_raw_msg.find("TXN1") != fsp::str_t::npos);
  CHECK(state->txn_raw_msg.find("CdtTrfTxInf") != fsp::str_t::npos);
}

// --- Scenario 8: on_doc_stored() fires exactly once per document, strictly before on_doc_close()
// for that same document, under BOTH pool_shard_count=1 and pool_shard_count>1 -- deliberately
// forces multiple flush batches per document (a small ok_block_flush_size against a many-txn
// document) so the per-document (not per-batch) counting in pipeline::record_segments_stored()
// actually gets exercised across more than one flush -----------------------------------------------
TEST_CASE("pipeline: on_doc_stored() fires exactly once, strictly before on_doc_close(), across shard counts",
          "[pipeline][stages][on-doc-stored]")
{
  const auto shard_count = GENERATE(std::size_t{1}, std::size_t{4});
  CAPTURE(shard_count);

  temp_dir_guard dir;
  // 25 transactions, flushed in batches of 4 (ok_block_flush_size below) -- 26 total segments
  // (header + 25 txns) is NOT a clean multiple of 4, so the final flush_results() end-of-thread
  // flush (see xml_worker::flush_results()) also gets exercised, not just threshold-triggered
  // flushes.
  constexpr int num_txns = 25;
  const auto    doc_path = dir.write("doc.xml", multi_txn_doc(num_txns));

  auto state = std::make_shared<shared_state>();
  state->size_doc_seqs(1);
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg_ex("test-doc-stored", /*num_of_workers=*/4, shard_count, /*ok_block_flush_size=*/4);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  CHECK_FALSE(state->doc_stored_contract_violated.load());
  CHECK_FALSE(state->doc_close_seq_contract_violated.load());
  CHECK(state->doc_stored_calls.load() == 1); // exactly once, not zero, not more
  CHECK(state->doc_close_calls.load() == 1);

  const int stored_seq = state->doc_stored_seq[0].load();
  const int close_seq  = state->doc_close_seq[0].load();
  REQUIRE(stored_seq >= 0);      // on_doc_stored() actually fired
  REQUIRE(close_seq >= 0);       // on_doc_close() actually fired
  CHECK(stored_seq < close_seq); // storage-completeness known STRICTLY before the document closes

  // Functional non-regression: every segment (header + all txns) actually made it through.
  CHECK(res->total_segments_ok(p->ds_dscr()) == static_cast<std::size_t>(num_txns + 1));
  CHECK(res->total_segments_error(p->ds_dscr()) == 0);

  // on_doc_close()'s own new segments_stored parameter (doc_counters::stored_count()) matches the
  // same total - every one of this document's segments was confirmed durably stored by the time
  // on_doc_close() ran (guaranteed by stored_seq < close_seq above), so this is not a partial tally.
  CHECK(state->doc_close_segments_stored.load() == num_txns + 1);
}

// --- Scenario 9: several documents, several worker threads, several shards at once -- confirms no
// on_doc_stored() fires early or twice under real concurrency, not just the single-document case
// above. Repeated a few times (CATCH's GENERATE over an iteration index) since a race, if one
// existed, would not necessarily reproduce on every run. ------------------------------------------
TEST_CASE("pipeline: on_doc_stored() ordering holds for multiple documents under real concurrency", "[pipeline][stages][on-doc-stored]")
{
  const auto iteration = GENERATE(range(0, 3));
  CAPTURE(iteration);

  temp_dir_guard           dir;
  constexpr int            num_docs     = 6;
  constexpr int            txns_per_doc = 12;
  std::vector<std::string> doc_paths;
  for (int d = 0; d < num_docs; ++d) doc_paths.push_back(dir.write(fmt::format("doc-{}.xml", d), multi_txn_doc(txns_per_doc)));

  auto state = std::make_shared<shared_state>();
  state->size_doc_seqs(num_docs);
  stage_test_hooks hooks(state);

  // 4 shards, 6 worker threads, small flush size -- segments from different documents will end up
  // interleaved within the same flush batches (see xml_worker::flush_ok_block()'s own doc comment
  // on why record_segments_stored() must group by doc_ndx), and different worker threads race to
  // be the one whose flush crosses each document's own segment-stored total.
  auto cfg      = make_cfg_ex("test-doc-stored-concurrent", /*num_of_workers=*/6, /*pool_shard_count=*/4, /*ok_block_flush_size=*/5);
  auto [p, res] = fsp::importer::exec(cfg, doc_paths, xsd_path(), hooks);

  REQUIRE(res.has_value());
  CHECK_FALSE(state->doc_stored_contract_violated.load());
  CHECK_FALSE(state->doc_close_seq_contract_violated.load());
  CHECK(state->doc_stored_calls.load() == num_docs);
  CHECK(state->doc_close_calls.load() == num_docs);

  for (int d = 0; d < num_docs; ++d)
  {
    CAPTURE(d);
    const int stored_seq = state->doc_stored_seq[static_cast<std::size_t>(d)].load();
    const int close_seq  = state->doc_close_seq[static_cast<std::size_t>(d)].load();
    REQUIRE(stored_seq >= 0);
    REQUIRE(close_seq >= 0);
    CHECK(stored_seq < close_seq);
  }

  CHECK(res->total_segments_ok(p->ds_dscr()) == static_cast<std::size_t>(num_docs * (txns_per_doc + 1)));
  CHECK(res->total_segments_error(p->ds_dscr()) == 0);
}

// --- Scenario 10: the header-priority mechanism is a compile-time property of fsp::work now (see
// reflection.hpp's seg_schema/hdr_seg_schema and work.hpp: pacs8_hdr permanently derives from
// hdr_seg_schema), not a runtime opt-in -- every TEST_CASE in this file already exercises it via
// make_cfg()/make_cfg_ex(). This one confirms functional correctness is unaffected (every segment
// still processed exactly once) and that the header segment's own on_type() is, in fact, observed
// to fire -- a SOFT scheduling signal, not a hard per-segment ordering guarantee (see
// docs/importer_usage.md's own "Header segments are processed first" section: the guarantee is
// "never starved behind an unbounded pile", not "always strictly first" under every possible
// thread interleaving), so this asserts the header was seen at all (hdr_raw_msg non-empty) rather
// than a strict happens-before relationship that could flake under real thread scheduling. -------
TEST_CASE("pipeline: hdr_seg_schema routes the header segment through the priority queue without breaking correctness",
          "[pipeline][stages][header-priority]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", multi_txn_doc(20));

  auto             state = std::make_shared<shared_state>();
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg_ex("test-header-priority", /*num_of_workers=*/4, /*pool_shard_count=*/2, /*ok_block_flush_size=*/3);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  CHECK(p->ds_dscr()[0].status().ok());
  CHECK(res->total_segments_ok(p->ds_dscr()) == 21); // header + 20 txns
  CHECK(res->total_segments_error(p->ds_dscr()) == 0);
  {
    const std::scoped_lock lock(state->raw_msg_mutex);
    CHECK(state->hdr_raw_msg.find("GrpHdr") != fsp::str_t::npos); // the header segment was, in fact, processed
  }
}

// --- Scenario 12: white-box proof, at the doc_cutter/segment_pool level, that EVERY hdr_seg_schema
// segment lands in the header queue and EVERY other segment lands in the ordinary queue -- Scenario
// 11 above only shows the header was processed at all, which a bug that dropped the header/ordinary
// distinction entirely (routing everything through ONE queue) would still pass. This test drives
// doc_cutter directly (no pipeline_worker, no P-role threads at all) so cutting is the only thing
// happening -- immediately after cut() returns, the two queues' contents are counted with nothing
// else able to have drained them yet, making the assertion exact rather than best-effort. ---------
TEST_CASE("doc_cutter: every hdr_seg_schema segment is routed into the header queue, every other "
          "segment into the ordinary queue",
          "[doc_cutter][header-priority]")
{
  const auto num_txns = GENERATE(1, 20);
  CAPTURE(num_txns);

  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", multi_txn_doc(num_txns));

  static const fsp::xerces_mgr xerces_life; // process-wide Xerces init/terminate, same role as importer's own -- see importer.hpp

  const auto log_cfg = silent_log_cfg("test-doc-cutter-header-routing");
  auto       log_ptr = logger::Logger::create(log_cfg);
  REQUIRE(log_ptr.has_value());

  fsp::doc_set_dscr ds_dscr(**log_ptr, 1);
  REQUIRE(ds_dscr.add_document(doc_path));
  REQUIRE(ds_dscr.set_grammar(xsd_path()));

  const auto cfg = fsp::importer_config{.targets        = fsp::proc_data_of<^^fsp::work>(),
                                        .num_of_workers = 1,
                                        .log_config     = log_cfg,
                                        .program_name   = "test-doc-cutter-header-routing"};
  // pacs8_hdr is seg_type 0, pacs8_txn is seg_type 1 (declaration order in work.hpp) -- confirms
  // the compile-time side of the mechanism (reflection.hpp's seg_schema::is_header()/
  // hdr_seg_schema, proc_data_of()) independently of the runtime routing this test goes on to
  // check below.
  REQUIRE(cfg.targets.is_header.size() == 2);
  CHECK(cfg.targets.is_header[0]);       // pacs8_hdr : hdr_seg_schema
  CHECK_FALSE(cfg.targets.is_header[1]); // pacs8_txn : seg_schema

  fsp::segment_pool pool(**log_ptr, /*no_of_slots=*/static_cast<std::size_t>(num_txns) + 1, /*num_shards=*/2);
  fsp::doc_cutter   cutter(cfg, **log_ptr, pool, ds_dscr);
  REQUIRE(cutter.init());
  REQUIRE(cutter.cut(0));

  // Nothing has run except cutting itself (no pipeline_worker/P-role thread was ever started), so
  // every one of this document's segments is sitting in exactly one of the two ready-queue sets
  // right now -- draining both to plain counts is exact, not an approximation racing another
  // thread.
  std::size_t header_count = 0;
  for (std::size_t shard = 0; shard < pool.num_shards(); ++shard)
    while (auto idx = pool.try_pop_ready_header(shard))
    {
      CHECK(pool.segment_at(*idx).subtree_type() == 0); // every header-queue entry is, in fact, seg_type 0 (pacs8_hdr)
      ++header_count;
    }
  std::size_t ordinary_count = 0;
  for (std::size_t shard = 0; shard < pool.num_shards(); ++shard)
    while (auto idx = pool.try_pop_ready(shard))
    {
      CHECK(pool.segment_at(*idx).subtree_type() == 1); // every ordinary-queue entry is, in fact, seg_type 1 (pacs8_txn)
      ++ordinary_count;
    }

  CHECK(header_count == 1); // exactly the one GrpHdr segment, never more, never fewer
  CHECK(ordinary_count == static_cast<std::size_t>(num_txns));
  CHECK(cutter.segments_found() == header_count + ordinary_count); // nothing lost, nothing double-counted
}

// --- Scenario 13: a HEADER segment's own on_type() returning false is error_class::he, not te --
// pipeline::check_segment_semantics() looks up the FAILING segment's own subtree_type() against
// cfg_.targets.is_header to tell the two apart (see its own doc comment) -- this proves that
// lookup actually reaches the right verdict for the header segment specifically, and that HE (like
// UA/SE/VE) fires on_remove_stored_data_safe() with no_headers=true (only the header record itself
// is not what needs removing -- see error_class::he's own doc comment). ---------------------------
TEST_CASE("pipeline: a failing header segment's on_type() is recorded as error_class::he, "
          "on_remove_stored_data_safe() fires with no_headers=true",
          "[pipeline][stages][error-class][HE]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state         = std::make_shared<shared_state>();
  state->hdr_verdict = false; // only the header segment's own on_type() fails
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-error-class-he", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  CHECK(ds_dscr[0].has_error(fsp::error_class::he));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::te));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ua));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::se));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ve));
  // error_mask()'s he bit is set here (per-segment, from check_segment_semantics()), independent
  // of doc_status_t::semantic_ itself: that three_state fact is set separately, once, from
  // maybe_finish_seg_processing() - which now reports semantic_ok=false (not the hook's own
  // verdict) whenever the document is already_rejected, exactly the fix that lets done_ reach its
  // k_done_threshold short-circuit for a document rejected on error_class::he specifically (see
  // that method's own doc comment in pipeline.cpp for the full "stuck at item_state_id::loading
  // forever" bug this closes) - stage_test_hooks::on_doc_sem_check() itself is never even called
  // here (already_rejected is true by the time this runs), so its own default-true verdict plays
  // no part in why semantic_status() ends up invalid.
  CHECK(ds_dscr[0].status().semantic_status() == fsp::three_state::invalid);
  CHECK(ds_dscr[0].rejected());

  CHECK(state->remove_stored_data_calls.load() == 1);
  CHECK(state->remove_stored_data_out_doc_id.load() == ds_dscr[0].out_doc_id());
  CHECK(state->remove_stored_data_no_headers.load()); // true for HE specifically
}

// --- Scenario 14: a single failing TRANSACTION (non-header) segment is error_class::te -- unlike
// UA/SE/VE/HE above, a lone TE does NOT reject the whole document (see check_segment_semantics()'s
// own doc comment: only the failing segment itself is recorded as semantically wrong, folded into
// per-segment error tracking, not doc_status_t::semantic_ -- that fact is set later, once, from
// on_doc_sem_check()'s own document-wide verdict), so on_remove_stored_data_safe() must NOT fire
// for TE alone. --------------------------------------------------------------------------------
TEST_CASE("pipeline: a failing non-header segment's on_type() is recorded as error_class::te, "
          "on_remove_stored_data_safe() does NOT fire for TE alone",
          "[pipeline][stages][error-class][TE]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state         = std::make_shared<shared_state>();
  state->txn_verdict = false; // only the transaction segment's own on_type() fails
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-error-class-te", 2);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  CHECK(ds_dscr[0].has_error(fsp::error_class::te));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::he));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ua));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::se));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::ve));

  // TE alone (no on_doc_sem_check() override here, default verdict stays true) does not reject the
  // document via doc_status_t::semantic_ -- consistent with docs/importer_usage.md's own "Document
  // errors" section: a single failed transaction is recorded per-segment (already visible in
  // total_segments_error() below), not folded into the document-wide verdict.
  CHECK(ds_dscr[0].status().semantic_status() == fsp::three_state::valid);
  CHECK_FALSE(ds_dscr[0].rejected());
  CHECK(state->remove_stored_data_calls.load() == 0); // never fires for TE alone

  // Functional non-regression: the failed transaction segment is counted as an error, the header
  // as a successful segment.
  CHECK(res->total_segments_ok(ds_dscr) == 1);    // header only
  CHECK(res->total_segments_error(ds_dscr) == 1); // the one failed transaction
}

// --- Scenario 15: a header segment that fails on_type() (HE) now rejects the whole document
// EARLY (pipeline::report_error_class() calls mark_rejected() unconditionally, including for HE -
// see its own doc comment) - with num_of_workers=1 (deterministic, sequential C->P: the header
// segment is always cut/processed before any transaction segment of the same document), the
// transaction segment's own on_type() (which would otherwise ALSO fail here, txn_verdict=false)
// is never even reached: xml_worker::process_one() sees doc_dscr::rejected()==true already and
// skips it outright ("document invalid, segment skipped" - see that method's own doc comment),
// exactly the "P picks up a segment of an already-invalid document and releases it without
// running its own semantic check" behavior this change exists for. error_class::te therefore never
// gets recorded here - HE alone already won mark_error()'s own "first error class" race and fired
// on_remove_stored_data_safe() once, before the transaction segment could contribute its own bit.
TEST_CASE("pipeline: a header segment's on_type() failure (HE) rejects the document early enough "
          "that a later transaction segment is skipped, not itself recorded as TE",
          "[pipeline][stages][error-class]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state         = std::make_shared<shared_state>();
  state->hdr_verdict = false; // header fails -- HE
  state->txn_verdict = false; // would ALSO fail as TE, if ever reached (see this test's own doc comment - it is not)
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-error-class-multi", 1); // single worker: deterministic, sequential C->P order
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  CHECK(ds_dscr[0].has_error(fsp::error_class::he));
  CHECK_FALSE(ds_dscr[0].has_error(fsp::error_class::te));
  CHECK(ds_dscr[0].rejected());
  // Exactly one on_remove_stored_data_safe() call - HE's own report_error_class() call fired it,
  // no second call for the (never-recorded) TE.
  CHECK(state->remove_stored_data_calls.load() == 1);
}
// NOLINTEND(readability-magic-numbers)
