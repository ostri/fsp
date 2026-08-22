#include "pacs8_cb.hpp"
#include "work.hpp"
#include <fmt/format.h>
#include <magic_enum.hpp>

namespace
{
  // BIC codes of every agent in ach's own dic_agents reference table (see
  // ach/config/dic_agents.conf) -- kept here as a plain, semicolon-delimited literal instead of
  // reading that JSON file at runtime, since fsp only vendors ach/utility.hpp/.cpp (see
  // src/ach/), not ach's own DB-backed dic_agents loader. usr::bic_code_t::init() (see below)
  // needs exactly this: a caller-owned, stable buffer plus a delimiter, nothing more. Lives here,
  // not in work.hpp, since it's run-level validation data, not part of the document's own
  // segmentation schema (which is all work.hpp otherwise describes).
  // clang-format off
  constexpr fsp::cstr_t known_agent_bics =
    "HAABSI22;BAKOSI2X;KSPKSI22;SZKBSI2X;GORESI2X;LJBASI2X;KBMASI2X;"
    "SIDRSI22;BACXSI22;HDELSI22;HLONSI22;HKVISI22;BFKKSI22;BSLJSI2X";
  // clang-format on
} // namespace

fsp::e_void pacs8_cb::on_run_start(const fsp::doc_set_dscr& ds_dscr)
{
  // No need to chain to a base body here (unlike the old on_run_start()) -- pipeline_hooks'
  // own final on_run_safe_start() already stamped run_start_/log_ before calling this override.
  log().info(fmt::format("[pacs8_cb] {:12}: ds_dscr.size()={}", "on_run_start", ds_dscr.size()));

  // usr::bic_code_t::init() must run exactly once, on the main thread, strictly before any
  // worker thread starts materializing pacs8_txn::debtor_bic/creditor_bic (validated_t<bic_code_t>,
  // see work.hpp) -- on_run_start() is guaranteed to run first (see pipeline_hooks.hpp's own
  // class comment), so this is the right place, not e.g. main() itself, which would need to know
  // about a fsp::work implementation detail it otherwise has no reason to touch.
  usr::bic_code_t::init(known_agent_bics, ';');
  return {};
}

fsp::e_void pacs8_cb::on_run_end(const fsp::doc_set_counter&           counters,
                                 const fsp::doc_set_dscr&              ds_dscr,
                                 std::span<const fsp::pipeline_hooks*> worker_clones)
{
  const auto elapsed_sec = elapsed_run_sec();
  log().info(fmt::format("[pacs8_cb] {:12}: counters.total_docs()={} ds_dscr.size()={} worker_clones.size()={} elapsed={:.3f} sec",
                         "on_run_end",
                         counters.total_docs(),
                         ds_dscr.size(),
                         worker_clones.size(),
                         elapsed_sec));

  // documents_seen is this hook's own per-clone counter (see the class's own doc comment),
  // summed here across every worker clone plus this original instance. Segment ok/error counts
  // are NOT summed here: counters (pipeline's own doc_counters, already folding every
  // on_seg_sem_check() verdict as it happens -- see pipeline::record_segment_done()) already
  // carries the true, authoritative totals, so there's nothing left to sum from the clones.
  std::size_t total_docs = documents_seen;
  for (const auto* w : worker_clones)
  {
    // Safe: every element of worker_clones was made by cloning THIS SAME pacs8_cb instance
    // (see pipeline_hooks_crtp<pacs8_cb>::clone()), so it's always actually a pacs8_cb.
    const auto* clone = static_cast<const pacs8_cb*>(w); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
    total_docs += clone->documents_seen;
  }
  log().info(fmt::format("[pacs8_cb] {:12}: CUMULATIVE documents={} segments={} (ok={} error={}) total processing time={:.3f} sec",
                         "on_run_end",
                         total_docs,
                         counters.total_segments(ds_dscr),
                         counters.total_segments_ok(ds_dscr),
                         counters.total_segments_error(ds_dscr),
                         elapsed_sec));
  return {};
}

fsp::e_void pacs8_cb::on_wrk_start(int worker_id, fsp::cstr_t thread_name)
{
  // No need to chain to a base body here (unlike the old on_wrk_start()) -- pipeline_hooks'
  // own final on_wrk_safe_start() already stamped worker_start_/log_ before calling this override.
  log().info(fmt::format("[pacs8_cb] {:12}: worker_id={} thread_name='{}'", "on_wrk_start", worker_id, thread_name));
  return {};
}

fsp::e_void pacs8_cb::on_wrk_end(int worker_id, fsp::cstr_t thread_name)
{
  const auto elapsed_sec = elapsed_worker_sec();
  // Per-worker segment ok/error counts aren't tracked here (see the class's own doc comment) --
  // that granularity isn't exposed anywhere in pipeline_hooks; only the whole-run total is,
  // via on_run_end()'s own counters parameter.
  log().info(fmt::format("[pacs8_cb] {:12}: worker_id={} thread_name='{}' (documents_seen={}) thread runtime={:.3f} sec",
                         "on_wrk_end",
                         worker_id,
                         thread_name,
                         documents_seen,
                         elapsed_sec));
  return {};
}

fsp::e_void pacs8_cb::on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr)
{
  ++documents_seen;
  log().info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_open", doc_ndx, dscr.path()));
  return {};
}

fsp::e_void pacs8_cb::on_doc_cutting_end(std::size_t doc_ndx, const fsp::doc_dscr& dscr)
{
  log().info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_cutting_end", doc_ndx, dscr.path()));
  return {};
}

fsp::e_void pacs8_cb::on_doc_stored(std::size_t doc_ndx, const fsp::doc_dscr& dscr)
{
  log().info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_stored", doc_ndx, dscr.path()));
  return {};
}

fsp::e_void pacs8_cb::on_doc_finish(std::size_t doc_ndx)
{
  log().info(fmt::format("[pacs8_cb] {:12}: doc_ndx={}", "on_doc_finish", doc_ndx));
  return {};
}

bool pacs8_cb::on_doc_close(std::size_t doc_ndx, const fsp::doc_status_t& verdict, const fsp::error_info& err, const fsp::doc_dscr& dscr)
{
  log().info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} syntax={} validation={} semantic={} err='{}' path='{}'",
                         "on_doc_close",
                         doc_ndx,
                         static_cast<int>(verdict.syntax_status()),
                         static_cast<int>(verdict.valid_status()),
                         static_cast<int>(verdict.semantic_status()),
                         err.message(),
                         dscr.path()));
  return verdict.ok();
}

// The verdict returned by each on_type() overload below is folded into doc_counters by
// pipeline::record_segment_done() (the caller of typed_semantic_check's own generic
// on_seg_sem_check()) -- see the class's own doc comment on why this hook doesn't also
// keep its own ok/error counters.
bool pacs8_cb::on_type(const fsp::work::pacs8_hdr& hdr,
                       std::string_view /*raw_msg*/,
                       const fsp::doc_dscr& /*dscr*/,
                       fsp::segment_result& result,
                       bool                 is_first,
                       bool                 is_last) const
{
  // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
  const bool ok = (result.seg_id() % 2 == 0);
  log().debug(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} header: msg_id='{}' amount_sum={}",
                          "on_type",
                          result.seg_id(),
                          result.doc_ndx(),
                          is_first,
                          is_last,
                          ok,
                          hdr.msg_id,
                          hdr.amount_sum));
  return ok;
}

bool pacs8_cb::on_type(const fsp::work::pacs8_txn& txn,
                       std::string_view /*raw_msg*/,
                       const fsp::doc_dscr& /*dscr*/,
                       fsp::segment_result& result,
                       bool                 is_first,
                       bool                 is_last) const
{
  // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
  const bool       ok = (result.seg_id() % 2 == 0);
  const fsp::str_t iban_str =
    txn.debtor_iban ? txn.debtor_iban->value : fmt::format("<invalid: {}>", result.errors()[txn.debtor_iban.error()].to_string());
  const fsp::str_t bic_str =
    txn.debtor_bic ? txn.debtor_bic->value : fmt::format("<invalid: {}>", result.errors()[txn.debtor_bic.error()].to_string());
  log().debug(
    fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} txn: txn_id='{}' debtor_iban={} debtor_bic={}",
                "on_type",
                result.seg_id(),
                result.doc_ndx(),
                is_first,
                is_last,
                ok,
                txn.txn_id,
                iban_str,
                bic_str));
  return ok;
}
