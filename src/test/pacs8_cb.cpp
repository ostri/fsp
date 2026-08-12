#include "pacs8_cb.hpp"
#include "work.hpp"
#include <fmt/format.h>
#include <magic_enum.hpp>
#include <type_traits>
#include <variant>

void pacs8_cb::on_run_start(const fsp::doc_set_dscr& ds_dscr, const logger::Logger& log)
{
  pipeline_hooks::on_run_start(ds_dscr, log); // stamps run_start_ for elapsed_run_sec()
  log.info(fmt::format("[pacs8_cb] {:12}: ds_dscr.size()={}", "on_run_start", ds_dscr.size()));
}

void pacs8_cb::on_run_end(const fsp::doc_set_counter&           counters,
                          const fsp::doc_set_dscr&              ds_dscr,
                          std::span<const fsp::pipeline_hooks*> worker_clones,
                          const logger::Logger&                 log)
{
  const auto elapsed_sec = elapsed_run_sec();
  log.info(fmt::format("[pacs8_cb] {:12}: counters.total_docs()={} ds_dscr.size()={} worker_clones.size()={} elapsed={:.3f} sec",
                       "on_run_end",
                       counters.total_docs(),
                       ds_dscr.size(),
                       worker_clones.size(),
                       elapsed_sec));

  // IIFE (rather than three mutable accumulators) so total_docs/total_ok/total_err stay
  // const-correct -- each is computed once, not mutated across the loop.
  const auto sum_over_clones = [&](std::size_t pacs8_cb::* field) -> std::size_t
  {
    std::size_t total = this->*field;
    for (const auto* w : worker_clones)
    {
      // Safe: every element of worker_clones was made by cloning THIS SAME pacs8_cb instance
      // (see pipeline_hooks_crtp<pacs8_cb>::clone()), so it's always actually a pacs8_cb.
      const auto* clone = static_cast<const pacs8_cb*>(w); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
      total += clone->*field;
    }
    return total;
  };
  const std::size_t total_docs = sum_over_clones(&pacs8_cb::documents_seen);
  const std::size_t total_ok   = sum_over_clones(&pacs8_cb::segments_ok);
  const std::size_t total_err  = sum_over_clones(&pacs8_cb::segments_error);
  log.info(fmt::format("[pacs8_cb] {:12}: CUMULATIVE documents={} segments={} (ok={} error={}) total processing time={:.3f} sec",
                       "on_run_end",
                       total_docs,
                       total_ok + total_err,
                       total_ok,
                       total_err,
                       elapsed_sec));
}

void pacs8_cb::on_wrk_start(int worker_id, fsp::cstr_t thread_name, const logger::Logger& log)
{
  pipeline_hooks::on_wrk_start(worker_id, thread_name, log); // stamps worker_start_ for elapsed_worker_sec()
  log.info(fmt::format("[pacs8_cb] {:12}: worker_id={} thread_name='{}'", "on_wrk_start", worker_id, thread_name));
}

void pacs8_cb::on_wrk_end(int worker_id, fsp::cstr_t thread_name, const logger::Logger& log)
{
  const auto elapsed_sec = elapsed_worker_sec();
  log.info(fmt::format(
    "[pacs8_cb] {:12}: worker_id={} thread_name='{}' (documents_seen={} segments_seen={} ok={} error={}) thread runtime={:.3f} sec",
    "on_wrk_end",
    worker_id,
    thread_name,
    documents_seen,
    segments_ok + segments_error,
    segments_ok,
    segments_error,
    elapsed_sec));
}

void pacs8_cb::on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const logger::Logger& log)
{
  ++documents_seen;
  log.info(fmt::format("[pacs8_cb] {:12}: doc_ndx={} path='{}'", "on_doc_open", doc_ndx, dscr.path()));
}

void pacs8_cb::on_doc_close(std::size_t doc_ndx, fsp::doc_status status, const fsp::doc_dscr& dscr, const logger::Logger& log)
{
  log.info(
    fmt::format("[pacs8_cb] {:12}: doc_ndx={} status={} path='{}'", "on_doc_close", doc_ndx, magic_enum::enum_name(status), dscr.path()));
}

bool pacs8_cb::process_header(const fsp::work::pacs8_header& hdr,
                              const fsp::segment_result&     result,
                              bool                           is_first,
                              bool                           is_last,
                              const logger::Logger&          log) const
{
  // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
  const bool ok = (result.seg_id() % 2 == 0);
  log.debug(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} header: msg_id='{}' amount_sum={}",
                        "on_seg_proc",
                        result.seg_id(),
                        result.doc_ndx(),
                        is_first,
                        is_last,
                        ok,
                        hdr.msg_id,
                        hdr.amount_sum));
  return ok;
}

bool pacs8_cb::process_txn(const fsp::work::pacs8_txn& txn,
                           const fsp::segment_result&  result,
                           bool                        is_first,
                           bool                        is_last,
                           const logger::Logger&       log) const
{
  // Artificial rule for this demo: every ODD seg_id is a semantic error, every EVEN is ok.
  const bool       ok = (result.seg_id() % 2 == 0);
  const fsp::str_t iban_str =
    txn.debtor_iban ? txn.debtor_iban->value : fmt::format("<invalid: {}>", result.errors()[txn.debtor_iban.error()].to_string());
  log.debug(fmt::format("[pacs8_cb] {:12}: seg_id={} doc_ndx={} is_first={} is_last={} ok={} txn: txn_id='{}' debtor_iban={}",
                        "on_seg_proc",
                        result.seg_id(),
                        result.doc_ndx(),
                        is_first,
                        is_last,
                        ok,
                        txn.txn_id,
                        iban_str));
  return ok;
}

bool pacs8_cb::on_seg_proc([[maybe_unused]] const fsp::xml_segment& segment,
                           fsp::segment_result&                     result,
                           bool                                     is_first,
                           bool                                     is_last,
                           const logger::Logger&                    log)
{
  // materialize_variant() hands back the segment as the developer's own work.hpp schema type
  // (pacs8_header or pacs8_txn) instead of the generic, name-indexed result_values. result is
  // passed by reference (not result.values()) because a failed validated_t<X> field appends its
  // error_info to result.errors() during materialization.
  auto       seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result);
  const bool ok  = std::visit(
    [&]<typename T>(const T& s) -> bool
    {
      if constexpr (std::is_same_v<T, fsp::work::pacs8_header>) return process_header(s, result, is_first, is_last, log);
      else if constexpr (std::is_same_v<T, fsp::work::pacs8_txn>) return process_txn(s, result, is_first, is_last, log);
      else static_assert(sizeof(T) == 0, "on_seg_proc: unhandled fsp::work schema type -- add a branch above");
    },
    seg);

  if (ok) [[likely]]
    ++segments_ok;
  else ++segments_error;
  return ok;
}