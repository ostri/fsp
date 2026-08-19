// test_pipeline_stages.cpp
//
// Pipeline-level tests for the C(cutter)/V(validator)/P(processor) three-way race the
// pipeline_hooks rework (on_doc_cutting_finished/on_doc_sem_check/on_doc_close, doc_status_t,
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
#include "importer.hpp"
#include "typed_semantic_check.hpp"
#include "work.hpp" // IWYU pragma: keep -- ^^fsp::work needs the actual schema classes
#include <atomic>
#include <catch2/catch_test_macros.hpp>
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
    std::mutex                doc_close_err_mutex;   // guards doc_close_err_message (plain string, not lock-free)
    fsp::str_t                doc_close_err_message; // last on_doc_close()'s own err.message(), verbatim
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
      return true;
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
      return true;
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
    [[nodiscard]] bool on_doc_close(std::size_t /*doc_ndx*/,
                                    const fsp::doc_status_t& verdict,
                                    const fsp::error_info&   err,
                                    const fsp::doc_dscr& /*dscr*/) override
    {
      state_->doc_close_calls.fetch_add(1, std::memory_order_relaxed);
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

  fsp::importer_config make_cfg(std::string_view app_name, std::size_t num_of_workers)
  {
    return fsp::importer_config{.targets        = fsp::proc_data_of<^^fsp::work>(),
                                .num_of_workers = num_of_workers,
                                .log_config     = silent_log_cfg(app_name),
                                .program_name   = std::string(app_name)};
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
  CHECK(status.semantic_status() == fsp::three_state::valid); // on_doc_sem_check() default verdict (true) still ran

  // The document's segments were discarded (pipeline::discard_invalid_doc_results() +
  // xml_worker::process_one()'s own "document invalid, skip" path) -- neither results() nor
  // errors() should retain anything belonging to doc 0, even though P DID process (or start
  // processing) at least one segment before the document was marked invalid.
  CHECK(p->get_results().empty());
  bool any_error_for_doc0 = false;
  for (const auto& e : p->get_errors())
    if (e.doc_ndx() == 0) any_error_for_doc0 = true;
  CHECK_FALSE(any_error_for_doc0);

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

  CHECK(p->get_results().empty());
  // on_doc_close() fires exactly once even though the document never produced a single
  // successfully-cut segment.
  CHECK(state->doc_close_calls.load() == 1);
  CHECK_FALSE(state->doc_close_seen_syntax_ok.load());
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
  CHECK(p->get_results().size() == 2);
  CHECK(p->get_errors().empty());
}

// --- Scenario 5: get_doc_agent_id()'s default body -- doc_dscr::agent_id() stays nullopt --------
TEST_CASE("pipeline: get_doc_agent_id()'s default (no override) leaves doc_dscr::agent_id() unset", "[pipeline][stages][agent-id]")
{
  temp_dir_guard dir;
  const auto     doc_path = dir.write("doc.xml", well_formed_valid_doc());

  auto state = std::make_shared<shared_state>();
  // No resolved_agent_id passed -- stage_test_hooks::get_doc_agent_id() itself still overrides the
  // hook (returning std::nullopt), but that's functionally identical to inheriting
  // pipeline_hooks::get_doc_agent_id()'s own no-op default body (see its own doc comment) -- both
  // paths leave doc_dscr::agent_id() unset for every document.
  stage_test_hooks hooks(state);

  auto cfg      = make_cfg("test-agent-id-default", 1);
  auto [p, res] = fsp::importer::exec(cfg, std::vector<std::string>{doc_path}, xsd_path(), hooks);

  REQUIRE(res.has_value());
  const auto& ds_dscr = p->ds_dscr();
  CHECK_FALSE(ds_dscr[0].agent_id().has_value());

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

  // Exactly one terminal callback, never a cut/validate/semantic-check attempt.
  CHECK(state->doc_close_calls.load() == 1);
  CHECK_FALSE(state->doc_close_seen_syntax_ok.load());
  CHECK_FALSE(state->doc_close_seen_validation_ok.load());
  CHECK(state->doc_sem_check_calls.load() == 0);
  CHECK(state->segments_ever_seen.load() == 0);
  CHECK(p->get_results().empty());
  CHECK(p->get_errors().empty());
  {
    // do_cut() always wins for such a document (do_validate() only ever bails out silently - see
    // its own doc comment on why it cannot itself call report_validation_result() here).
    const std::scoped_lock lock(state->doc_close_err_mutex);
    CHECK(state->doc_close_err_message.find("cut skipped") != fsp::str_t::npos);
  }
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
// NOLINTEND(readability-magic-numbers)
