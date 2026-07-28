#include "pacs8_cb.hpp"
#include "work.hpp"
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <type_traits>
#include <variant>

void pacs8_cb::on_run_start(const fsp::doc_set_dscr& ds_dscr, const fsp::fsp_logger& log)
{
  run_start_ = std::chrono::steady_clock::now();
  log.info(fmt::format("[pacs8_cb] {:12}: ds_dscr.size()={}", "on_run_start", ds_dscr.size()));
}

void pacs8_cb::on_run_end(const fsp::doc_set_counter&           counters,
                          const fsp::doc_set_dscr&              ds_dscr,
                          std::span<const fsp::pipeline_hooks*> worker_clones,
                          const fsp::fsp_logger&                log)
{
  const auto elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - run_start_).count();
  log.info(fmt::format("[pacs8_cb] {:12}: counters.total_docs()={} ds_dscr.size()={} worker_clones.size()={} elapsed={:.3f} sec",
                       "on_run_end",
                       counters.total_docs(),
                       ds_dscr.size(),
                       worker_clones.size(),
                       elapsed_sec));

  std::size_t total_docs = documents_seen;
  std::size_t total_segs = segments_seen;
  std::size_t total_ok   = segments_ok;
  std::size_t total_err  = segments_error;
  for (const auto* w : worker_clones)
  {
    // Safe: every element of worker_clones was made by cloning THIS SAME pacs8_cb instance
    // (see pipeline_hooks_crtp<pacs8_cb>::clone()), so it's always actually a pacs8_cb.
    const auto* clone = static_cast<const pacs8_cb*>(w); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
    total_docs += clone->documents_seen;
    total_segs += clone->segments_seen;
    total_ok += clone->segments_ok;
    total_err += clone->segments_error;
  }
  log.info(fmt::format("[pacs8_cb] {:12}: CUMULATIVE documents={} segments={} (ok={} error={}) total processing time={:.3f} sec",
                       "on_run_end",
                       total_docs,
                       total_segs,
                       total_ok,
                       total_err,
                       elapsed_sec));
}

void pacs8_cb::on_wrk_start(int worker_id, fsp::cstr_t thread_name, const fsp::fsp_logger& log)
{
  worker_start_ = std::chrono::steady_clock::now();
  log.info(fmt::format("[pacs8_cb] {:12}: worker_id={} thread_name='{}'", "on_wrk_start", worker_id, thread_name));
}

void pacs8_cb::on_wrk_end(int worker_id, fsp::cstr_t thread_name, const fsp::fsp_logger& log)
{
  const auto elapsed_sec =
    std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - worker_start_).count();
  log.info(fmt::format(
    "[pacs8_cb] {:12}: worker_id={} thread_name='{}' (documents_seen={} segments_seen={} ok={} error={}) thread runtime={:.3f} sec",
    "on_wrk_end",
    worker_id,
    thread_name,
    documents_seen,
    segments_seen,
    segments_ok,
    segments_error,
    elapsed_sec));
}

void pacs8_cb::on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log)
{
  ++documents_seen;
  log.info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_open", doc_ndx, dscr.path()));
}

void pacs8_cb::on_doc_close(std::size_t doc_ndx, fsp::doc_status status, const fsp::doc_dscr& dscr, const fsp::fsp_logger& log)
{
  log.info(
    fmt::format("[pacs8_cb] {:12}: doc_ndx={} status={} path='{}'", "on_doc_close", doc_ndx, magic_enum::enum_name(status), dscr.path()));
}

bool pacs8_cb::on_seg_proc([[maybe_unused]] const fsp::xml_segment& segment,
                           const fsp::segment_result&               result,
                           bool                                     is_first,
                           bool                                     is_last,
                           const fsp::fsp_logger&                   log)
{
  ++segments_seen;
  const auto seg_id = result.seg_id();
  // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
  const bool ok = (seg_id % 2 == 0);
  if (ok) ++segments_ok;
  else ++segments_error;

  // materialize_variant() hands back the segment as the developer's own work.hpp schema type
  // (pacs8_header or pacs8_txn) instead of the generic, name-indexed result_values.
  auto seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result.values());
  std::visit(
    [&]<typename T>(const T& s)
    {
      if constexpr (std::is_same_v<T, fsp::work::pacs8_header>)
      {
        log.info(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} header: msg_id='{}' amount_sum={}",
                             "on_seg_proc",
                             seg_id,
                             result.doc_ndx(),
                             is_first,
                             is_last,
                             ok,
                             s.msg_id,
                             s.amount_sum));
      }
      else if constexpr (std::is_same_v<T, fsp::work::pacs8_txn>)
      {
        log.info(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} txn: txn_id='{}' debtor_iban={}",
                             "on_seg_proc",
                             seg_id,
                             result.doc_ndx(),
                             is_first,
                             is_last,
                             ok,
                             s.txn_id,
                             s.debtor_iban));
      }
      else static_assert(sizeof(T) == 0, "on_seg_proc: unhandled fsp::work schema type -- add a branch above");
    },
    seg);
  return ok;
}