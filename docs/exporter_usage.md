# Exporter usage

## Table of contents

1. [What it is for](#1-what-it-is-for)
2. [How to use it](#2-how-to-use-it)
   1. [Simple usage example](#21-simple-usage-example)
   2. [API description](#22-api-description)
      1. [`fsp::exporter<T, Q>`](#221-fspexportert-q)
      2. [`exporter_config_t`](#222-exporter_config_t)
      3. [`transaction_t` and `qualificators_t`](#223-transaction_t-and-qualificators_t)
      4. [Result and error types](#224-result-and-error-types)
      5. [Run statistics](#225-run-statistics)
   3. [Callback class description](#23-callback-class-description)
      1. [Method purpose and call order](#231-method-purpose-and-call-order)
      2. [Threading model](#232-threading-model)
      3. [Logging](#233-logging)
      4. [Error handling: everything is fatal](#234-error-handling-everything-is-fatal)
3. [Complex example](#3-complex-example)

## 1. What it is for

`fsp::exporter<T, Q>` produces batches of XML documents out of a stream of transactions, in
parallel, across several independent recipients ("drains") at once. A drain is whatever you are
exporting *to* -- typically a bank, identified by a BIC, but the exporter itself never assigns it
any particular meaning: it only knows a drain by a numeric id and a name.

Concretely, it is the mirror image of `fsp::importer` (see [importer_usage.md](importer_usage.md)):
where the importer reads large XML documents in and splits them into pieces for parallel
processing, the exporter takes a stream of transaction records (however you produce them -- a
database cursor, a queue, a file) and assembles them back into XML documents, several drains and
several documents at a time, using as many worker threads as you configure.

Each document is built up as: one header, one `<txn>`-shaped fragment per transaction (accumulated
in blocks of at most `max_doc_txn` transactions), one footer. The exporter handles everything
around that -- picking which drain a worker thread should work on next, allocating document ids,
staging each document under a temporary name, and atomically moving it to its final destination
only once it is known to be complete and accepted. You only ever have to answer four questions,
through a small set of callback methods: what should this document be named, how many transactions
are left for this drain, what is the next block of transactions, and what does one transaction /
the header / the footer look like as XML text.

## 2. How to use it

Everything a caller needs lives in `exporter.hpp` (the entry point), `exporter_types.hpp` (the
config/domain types), `exporter_error.hpp` (the error vocabulary, and the module's own id type
aliases), and `cb_exporter.hpp` (the callback interface you implement).

### 2.1. Simple usage example

```cpp
#include "exporter.hpp"

// 1. Your own transaction type -- one plain field is enough for this example.
struct my_txn : fsp::transaction_t
{
  // id, type, value are inherited from transaction_t; add your own fields as needed.
};

// 2. Your own run-qualifiers type -- carries whatever context every callback call needs
//    (a date range, a run mode, ...). Empty is fine if you don't need any.
struct my_qual : fsp::qualificators_t
{
};

// 3. Your callback: derive from cb_exporter_crtp<Derived, T, Q> and implement every method.
//    (see 2.3/3 below for what each one does -- this is the minimum that compiles and runs.)
//    Note the constructor: it must forward its own logger::Logger argument to cb_exporter_crtp's
//    base-class initializer list -- see 2.3.3.
class my_cb : public fsp::cb_exporter_crtp<my_cb, my_txn, my_qual>
{
public:
  explicit my_cb(const logger::Logger& log)
  : cb_exporter_crtp<my_cb, my_txn, my_qual>(log)
  {
  }

  fsp::exp_result<fsp::str_t> fetch_doc_name(const my_qual&, fsp::cstr_t, fsp::drain_t drain_id,
                                             blk_id_t block_number, blk_id_t, fsp::cstr_t prefix, fsp::cstr_t ext) override
  { return fmt::format("{}_{}_{}.{}", prefix, drain_id, block_number, ext); }

  fsp::exp_result<run_stat_t> fetch_run_stat(const my_qual&, fsp::drain_t /*drain_id*/) override
  { return run_stat_t{.remaining_txn_count = 100, .existing_doc_count = 0}; } // ask your own data source

  fsp::fetch_doc_data_result_t<my_txn> fetch_doc_data(const my_qual&, fsp::drain_t /*drain_id*/, fsp::doc_id_t) override
  { return {.status = fsp::fetch_doc_data_status::no_more_data, .block = {}, .error = {}}; } // ask your own data source

  fsp::exp_result<fsp::str_t> prepare_transaction(std::size_t, fsp::drain_t, fsp::doc_id_t, const my_txn& t) override
  { return fmt::format("<txn id=\"{}\">{}</txn>", fsp::to_string(t.id), t.value); }

  fsp::exp_result<fsp::str_t> prepare_header(const my_qual&, fsp::drain_t drain_id, fsp::doc_id_t doc_id, const blk_t&) override
  { return fmt::format(R"(<?xml version="1.0" encoding="UTF-8"?><doc drain="{}" id="{}">)", drain_id, doc_id); }

  fsp::exp_result<fsp::str_t> prepare_footer(const my_qual&, fsp::drain_t, fsp::doc_id_t, const blk_t&) override
  { return fsp::str_t("</doc>"); }

  bool document_prepared(const my_qual&, fsp::drain_t /*drain_id*/, fsp::doc_id_t /*doc_id*/) override
  { return true; } // e.g. mark these transactions as exported in your own data source here
};

int main()
{
  auto cfg = fsp::exporter_config_t{
    .drain_list        = {{.id = 1, .name = "BANK1", .max_doc_txn = 500}},
    .number_of_threads = 4,
    .filename_prefix    = "pacs008",
    .filename_ext       = "xml",
    .tmp_dir            = "/var/spool/export/tmp",
    .target_dir         = "/var/spool/export/out",
    .error_dir          = "/var/spool/export/error",
  };

  fsp::exporter<my_txn, my_qual> exp(cfg, my_qual{}, log, "my-exporter");
  my_cb                          cb(log);
  auto                           res = exp.exec(cb);
  if (! res) { /* res.error().to_string() */ }
  // res->total_documents, res->total_transactions, res->elapsed_ms
}
```

What actually happens, in order: `exec()` starts `number_of_threads` worker threads, each cloning
`cb` once for its own exclusive use. Every worker repeatedly picks a drain with work left, asks
your callback for the next block of transactions, writes header + transactions + footer to a
staged file, hands it to `document_prepared()`, and -- if accepted -- atomically moves it into
`target_dir`. This repeats until every drain reports `no_more_data`. Once every worker thread has
finished, `exec()` returns the aggregated statistics (or the first fatal error any worker hit).

### 2.2. API description

#### 2.2.1. `fsp::exporter<T, Q>`

```cpp
template <transaction_like T, qualifiers_like Q>
class exporter
{
public:
  exporter(exporter_config_t cfg, Q qualifiers, const logger::Logger& log, str_t parent_log_name);

  [[nodiscard]] exp_result<exporter_run_stats_t> exec(cb_exporter<T, Q>& proto_cb);
  [[nodiscard]] const exporter_state& state() const noexcept;
};
```

- `T` -- your concrete transaction type; must derive from `fsp::transaction_t` (see
  [2.2.3](#223-transaction_t-and-qualificators_t)).
- `Q` -- your concrete run-qualifiers type; must derive from `fsp::qualificators_t`.
- Constructor -- `cfg` is copied in; `qualifiers` is the one instance shared (by const reference)
  across every worker thread for the whole run -- build it once, before calling the constructor.
- `exec(proto_cb)` -- runs the whole export synchronously (blocks until every worker thread has
  finished), and returns either `exporter_run_stats_t` or the first fatal error reported by any
  worker. `proto_cb` is never mutated directly -- `exec()` only ever calls `proto_cb.clone()`, once
  per worker thread (see [2.3.2](#232-threading-model)).
- `state()` -- read-only access to the run's shared bookkeeping (drain list, stop signal); mainly
  useful for tests and diagnostics, not needed for ordinary use.

Unlike `fsp::importer::exec()`, `exporter` has a public constructor -- you own the `exporter<T,Q>`
instance yourself and call `exec()` on it explicitly, rather than getting one back from a static
factory function.

#### 2.2.2. `exporter_config_t`

```cpp
struct exporter_config_t
{
  std::vector<exporter_drain_cfg_t> drain_list;
  std::size_t                       number_of_threads = 0;
  str_t                             filename_prefix;
  str_t                             filename_ext = "xml";
  str_t                             tmp_dir;
  str_t                             target_dir;
  str_t                             error_dir;
};

struct exporter_drain_cfg_t
{
  std::uint8_t id = 0;
  str_t        name;
  std::size_t  max_doc_txn = 0;
};
```

| Field                   | Meaning                                                                                                              |
| ----------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `drain_list`            | every recipient to export to in this run; must not be empty                                                          |
| `number_of_threads`     | worker threads to start; must not be zero                                                                            |
| `filename_prefix`       | passed through to `fetch_doc_name()`, e.g. a message-type prefix like `"pacs008"`                                    |
| `filename_ext`          | passed through to `fetch_doc_name()` as-is, no leading dot (e.g. `"xml"`); defaults to `"xml"`                       |
| `tmp_dir`               | staging area a document is fully written to before being accepted                                                    |
| `target_dir`            | final destination a successfully produced document is atomically moved to                                            |
| `error_dir`             | best-effort diagnostic drop location for a tmp file that was fully written but then failed to *move* to `target_dir` |
| `drain_cfg.id`          | unique id for this drain, used everywhere a callback method takes a `drain_id`                                       |
| `drain_cfg.name`        | short drain name, e.g. a BIC                                                                                         |
| `drain_cfg.max_doc_txn` | maximum number of transactions per document for this drain -- the block size `fetch_doc_data()` should aim to return |

`tmp_dir` and `target_dir` must share a filesystem: the move from one to the other relies on
`std::filesystem::rename()` being atomic, which only holds within a single filesystem. `error_dir`
is not a routing destination for a rejected document (see
[2.3.4](#234-error-handling-everything-is-fatal)) -- it only ever receives a document whose write
succeeded but whose subsequent move failed for a physical I/O reason.

`cb_exporter<T,Q>::fetch_doc_name()` has a default implementation (see
[2.3.1](#231-method-purpose-and-call-order)) that already combines `filename_prefix`/`filename_ext`
with the drain id/block numbers -- overriding it is only needed for a custom naming scheme.

#### 2.2.3. `transaction_t` and `qualificators_t`

```cpp
struct transaction_t
{
  txn_id_t id   = 0; // unique transaction id (snowflake)
  int      type = 0; // transaction type
  str_t    value;    // transaction content -- input to prepare_transaction()
  virtual ~transaction_t() = default;
};

struct qualificators_t
{
  int run_id = 0;
  virtual ~qualificators_t() = default;
};
```

Both are plain base structs you derive your own concrete types from -- add whatever fields your
own `prepare_transaction()`/`fetch_doc_data()` need. `exporter<T,Q>` never interprets a
transaction's or a qualifier's fields itself; only your own callback does. `txn_id_t` is a 64-bit
signed integer, with its own `fsp::to_string()` helper for logging/display.

#### 2.2.4. Result and error types

```cpp
template <typename T> using exp_result = std::expected<T, exp_error_info>;
using                       ev_result  = std::expected<void, exp_error_info>;
```

Every fallible callback method and `exec()` itself return one of these -- check them the same way
as `std::expected` anywhere else in this codebase:

```cpp
auto res = exp.exec(cb);
if (! res) { fmt::print("Export failed: {}\n", res.error().to_string()); }
```

`exp_error_info` carries an `exp_error` code (`file_open_failed`, `invalid_config`,
`fetch_doc_data_failed`, `document_rejected`, ... -- see `exporter_error.hpp` for the full list),
a human-readable message, and, where known, the `drain_t`/`doc_id_t` the failure happened on.

`exporter_error.hpp` also defines the module's own id type aliases, used throughout the callback
interface and `exporter_config_t`/`exporter_state`: `drain_t` (a drain/recipient id, currently
`int`) and `doc_id_t` (a document id, currently `unsigned int`). Use these aliases rather than the
underlying built-in types in your own callback signatures -- see [2.3](#23-callback-class-description).

#### 2.2.5. Run statistics

```cpp
struct exporter_run_stats_t
{
  std::size_t total_documents    = 0;
  std::size_t total_transactions = 0;
  double      elapsed_ms         = 0.0;
};
```

This is what `exec()` returns on success -- the sum of every worker thread's own document/
transaction counts, computed once all of them have joined. There is no failed-document counter:
every failure anywhere aborts the whole run (see
[2.3.4](#234-error-handling-everything-is-fatal)), so a worker's documents are always either fully
successful or the run never completes.

### 2.3. Callback class description

```cpp
template <transaction_like T, qualifiers_like Q>
class cb_exporter
{
public:
  explicit cb_exporter(const logger::Logger& log) noexcept;
  virtual ~cb_exporter() = default;

  using run_stat_t = run_stat_pair_t;
  using blk_t      = txn_block_t<T>;   // one document's block of transactions
  using blk_id_t   = unsigned int;     // block_number/total_blocks

  [[nodiscard]] virtual std::unique_ptr<cb_exporter> clone() const = 0;

  // Has a default implementation (combines filename_prefix/drain_id/block_number/
  // total_blocks/filename_ext) -- override only for a custom naming scheme.
  [[nodiscard]] virtual exp_result<str_t> fetch_doc_name(const Q& q, cstr_t path, drain_t drain_id,
                                                         blk_id_t block_number, blk_id_t total_blocks,
                                                         cstr_t filename_prefix, cstr_t filename_ext);
  [[nodiscard]] virtual exp_result<run_stat_t> fetch_run_stat(const Q& q, drain_t drain_id) = 0;
  [[nodiscard]] virtual fetch_doc_data_result_t<T> fetch_doc_data(const Q& q, drain_t drain_id, doc_id_t doc_id) = 0;
  [[nodiscard]] virtual exp_result<str_t> prepare_transaction(std::size_t ndx, drain_t drain_id, doc_id_t doc_id, const T& data) = 0;
  // Both prepare_header()/prepare_footer() have a default implementation (empty string) --
  // override only if your document format actually needs one.
  [[nodiscard]] virtual exp_result<str_t> prepare_header(const Q& q, drain_t drain_id, doc_id_t doc_id, const blk_t& block);
  [[nodiscard]] virtual exp_result<str_t> prepare_footer(const Q& q, drain_t drain_id, doc_id_t doc_id, const blk_t& block);
  [[nodiscard]] virtual bool document_prepared(const Q& q, drain_t drain_id, doc_id_t doc_id) = 0;
protected:
  [[nodiscard]] const logger::Logger& lg() const noexcept; // see 2.3.3
};
```

You do not derive from `cb_exporter<T, Q>` directly -- derive from
`cb_exporter_crtp<Derived, T, Q>` instead (as in [2.1](#21-simple-usage-example)'s `my_cb`), which
implements `clone()` for you via `Derived`'s own copy constructor. `Derived` must therefore stay
copy-constructible, and its own constructor must forward a `const logger::Logger&` argument to
`cb_exporter_crtp`'s base-class initializer list (see [2.3.3](#233-logging)).

#### 2.3.1. Method purpose and call order

For each document a worker thread produces, in order:

1. **`fetch_run_stat(q, drain_id)`** -- called once per drain (by whichever worker thread reaches
   that drain first), before its first document. Returns
   `{remaining_txn_count, existing_doc_count}`. `existing_doc_count` is honored as the starting
   point for that drain's document numbering, so a re-run after a crash continues numbering rather
   than overwriting previously produced documents.
2. **`fetch_doc_data(q, drain_id, doc_id)`** -- fetches the next block of up to `max_doc_txn`
   transactions for `drain_id`. Returns one of three outcomes via `fetch_doc_data_status`: `ok` (a
   valid block), `no_more_data` (this drain is exhausted -- the worker moves on to another drain),
   or `error`.
3. **`fetch_doc_name(q, path, drain_id, block_number, total_blocks, filename_prefix,
   filename_ext)`** -- asks for a file name for the document about to be built. The default
   implementation (no override needed for the common case) formats
   `<prefix>-<run_id>-<drain_id>-<block_number>-<total_blocks>.<ext>`. If the resulting name
   collides with a file already present in `path` (the staging directory), the worker retries with
   a numeric suffix appended before the extension, up to 10 attempts -- your callback is never told
   about a collision itself.
4. **`prepare_header(q, drain_id, doc_id, block)`**, then **`prepare_transaction(ndx, drain_id,
   doc_id, data)`** once per transaction in `block` (`ndx` is that transaction's index within the
   block), then **`prepare_footer(q, drain_id, doc_id, block)`** -- each returns the XML text
   fragment to append. `prepare_header()`/`prepare_footer()` default to an empty string; override
   either only if your document format needs one. These three are written to the staged file via
   `fsp::xml_writer`, in this order.
5. **`document_prepared(q, drain_id, doc_id)`** -- called once the staged file has been fully
   written. Returning `true` moves it atomically to `target_dir`; see
   [2.3.4](#234-error-handling-everything-is-fatal) for what `false` means.

#### 2.3.2. Threading model

You supply one "prototype" `cb_exporter` instance to `exec()`. It is never mutated directly --
`exec()` clones it exactly once per worker thread, via `clone()`, at that thread's startup, and
each worker thread exclusively owns and calls into its own clone for the rest of the run. This
means:

- No two threads ever call into the same `cb_exporter` instance -- you do not need to guard your
  own per-instance state (e.g. a per-thread file handle or connection) with a lock.
- Any state you *do* want shared across every clone (e.g. a shared connection pool, or the
  in-memory transaction queues in `test_exporter.cpp`'s `demo_cb`) must live behind its own
  `shared_ptr` + mutex, copied (by pointer) into every clone -- `clone()`'s copy of `*this` copies
  the `shared_ptr`, not the pointed-to data.
- `q` (the run qualifiers) is the single instance passed to `exporter<T,Q>`'s constructor, shared
  by const reference across every worker/clone -- it is read-only from a callback's point of view.

#### 2.3.3. Logging

`cb_exporter<T,Q>`'s constructor binds the instance to the run's `logger::Logger` (the same one
`exporter<T,Q>`/`exporter_worker<T,Q>` themselves log through), accessible from inside your own
overrides via the protected `lg()` accessor. A concrete `Derived`'s own constructor must forward
its own `const logger::Logger&` argument up through `cb_exporter_crtp`'s constructor:

```cpp
class my_cb : public fsp::cb_exporter_crtp<my_cb, my_txn, my_qual>
{
public:
  explicit my_cb(const logger::Logger& log)
  : cb_exporter_crtp<my_cb, my_txn, my_qual>(log)
  {
  }
  // ... lg() is now valid inside every override below, including on every clone() ...
};
```

`clone()` copies `*this` via `Derived`'s copy constructor, so the logger reference (a trivially
copyable pointer internally) is carried over to every worker thread's own clone automatically --
`clone()` itself needs no special handling for it.

#### 2.3.4. Error handling: everything is fatal

There is no per-document soft-failure path. Any of the following stops the *entire* run, not just
the document in progress:

- any callback method above returning an error (`exp_result`/`fetch_doc_data_status::error`),
- an I/O failure inside `fsp::xml_writer` while writing the staged file,
- `document_prepared()` returning `false`.

Whichever worker thread hits one of these logs it, records it as the run's fatal error, and signals
every other worker thread to stop at its next loop check (`exporter_state::request_stop()`). Once
every thread has stopped, `exec()` returns `std::unexpected(...)` with that first error --
`document_prepared() == false` is deliberately treated the same as a system error, not as "this one
document was invalid, try the next" (there is no move to `error_dir` for it either, since nothing
about the document failed physically; the file simply stays in `tmp_dir`, and no more documents get
produced after it).

## 3. Complex example

The example below exports pending transactions for several banks (drains) at once, reading pending
work from an in-memory per-drain queue (stand-in for a real database/queue) and writing pacs.008-
style documents. It mirrors the shape of `src/test/test_exporter.cpp`'s `demo_cb`, with comments
explaining each piece.

```cpp
#include "exporter.hpp"
#include <logger/logger.hpp>
#include <map>
#include <mutex>

// --- 1. Domain types -----------------------------------------------------------------------

// One outgoing payment transaction. `value` (inherited from transaction_t) is deliberately left
// unused here -- this example builds the XML fragment straight from the extra fields below
// instead, to show that transaction_t's own fields are only a convenience, not a requirement.
struct payment_txn : fsp::transaction_t
{
  std::string debtor_iban;
  std::string creditor_iban;
  std::string amount; // decimal string, e.g. "1250.00"
};

// This run's qualifiers: a business date, shared read-only by every worker thread/clone.
struct export_run_qual : fsp::qualificators_t
{
  std::string business_date; // e.g. "2026-08-22"
};

// --- 2. Shared, thread-safe backing store for the demo -------------------------------------

// Every cb_exporter clone (one per worker thread) shares THIS instance via shared_ptr -- see
// 2.3.2's "threading model" section for why per-clone state and shared state need different
// treatment. A real implementation would replace `pending`/`existing_docs` with actual database
// queries, still funneled through the same mutex-guarded shape if the underlying client isn't
// itself thread-safe.
struct export_backend
{
  std::mutex                                       mtx;
  std::map<fsp::drain_t, std::vector<payment_txn>> pending;       // drain_id -> queue, front = next
  std::map<fsp::drain_t, std::size_t>              existing_docs; // drain_id -> docs already produced
};

// --- 3. The callback -------------------------------------------------------------------------

class payment_export_cb : public fsp::cb_exporter_crtp<payment_export_cb, payment_txn, export_run_qual>
{
public:
  payment_export_cb(std::shared_ptr<export_backend> backend, const logger::Logger& log)
  : cb_exporter_crtp<payment_export_cb, payment_txn, export_run_qual>(log)
  , backend_(std::move(backend))
  {
  }

  // fetch_doc_name() is NOT overridden here -- the default implementation (prefix-run_id-
  // drain_id-block_number-total_blocks.ext, see cb_exporter's own doc comment) is enough for
  // this example; override it only for a custom naming scheme.

  // existing_doc_count lets a re-run after a crash continue numbering instead of overwriting
  // documents a previous, interrupted run already produced.
  fsp::exp_result<run_stat_t> fetch_run_stat(const export_run_qual& /*qualifiers*/, fsp::drain_t drain_id) override
  {
    const std::scoped_lock lock(backend_->mtx);
    const auto              existing = backend_->existing_docs.contains(drain_id) ? backend_->existing_docs.at(drain_id) : 0;
    return run_stat_t{.remaining_txn_count = backend_->pending[drain_id].size(), .existing_doc_count = existing};
  }

  // Real code would query "next up to max_doc_txn pending rows for drain_id", ordered so a
  // partial final block is naturally the last one returned. drain_dscr_t::max_doc_txn is not
  // passed here directly -- fetch_doc_data() may return fewer, and the worker itself detects a
  // partial (< max_doc_txn) block as this drain's last one.
  fsp::fetch_doc_data_result_t<payment_txn> fetch_doc_data(const export_run_qual& /*qualifiers*/,
                                                           fsp::drain_t            drain_id,
                                                           fsp::doc_id_t           /*doc_id*/) override
  {
    static constexpr std::size_t BLOCK_SIZE = 100;
    const std::scoped_lock       lock(backend_->mtx);

    auto& queue = backend_->pending[drain_id];
    if (queue.empty()) { return {.status = fsp::fetch_doc_data_status::no_more_data, .block = {}, .error = {}}; }

    const std::size_t take = std::min(BLOCK_SIZE, queue.size());
    fsp::txn_block_t<payment_txn> block(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
    queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(take));
    return {.status = fsp::fetch_doc_data_status::ok, .block = std::move(block), .error = {}};
  }

  // Called once per transaction in the current block -- ndx is this transaction's own index
  // within that block (0-based), independent of doc_id/drain_id.
  fsp::exp_result<fsp::str_t> prepare_transaction(std::size_t          /*ndx*/,
                                                  fsp::drain_t          /*drain_id*/,
                                                  fsp::doc_id_t         /*doc_id*/,
                                                  const payment_txn&    data) override
  {
    return fmt::format(
      R"(<CdtTrfTxInf><PmtId><TxId>{}</TxId></PmtId><Amt><InstdAmt Ccy="EUR">{}</InstdAmt></Amt>)"
      R"(<DbtrAcct><Id><IBAN>{}</IBAN></Id></DbtrAcct><CdtrAcct><Id><IBAN>{}</IBAN></Id></CdtrAcct></CdtTrfTxInf>)",
      fsp::to_string(data.id), data.amount, data.debtor_iban, data.creditor_iban);
  }

  fsp::exp_result<fsp::str_t> prepare_header(const export_run_qual&    qualifiers,
                                             fsp::drain_t               drain_id,
                                             fsp::doc_id_t              doc_id,
                                             const blk_t&               block) override
  {
    return fmt::format(
      R"(<?xml version="1.0" encoding="UTF-8"?><Document><FIToFICstmrCdtTrf><GrpHdr>)"
      R"(<MsgId>{}-{}-{}</MsgId><CreDtTm>{}</CreDtTm><NbOfTxs>{}</NbOfTxs></GrpHdr>)",
      qualifiers.business_date, drain_id, doc_id, qualifiers.business_date, block.size());
  }

  fsp::exp_result<fsp::str_t> prepare_footer(const export_run_qual& /*qualifiers*/,
                                             fsp::drain_t            /*drain_id*/,
                                             fsp::doc_id_t           /*doc_id*/,
                                             const blk_t& /*block*/) override
  {
    return fsp::str_t("</FIToFICstmrCdtTrf></Document>");
  }

  // Everything succeeded and the document is now durably staged -- this is the point to mark
  // these transactions as exported in your own data source (e.g. an UPDATE ... WHERE id IN (...)
  // inside the same transaction the fetch_doc_data() rows came from). Returning false here is
  // fatal to the WHOLE run, not just this document -- see 2.3.4.
  bool document_prepared(const export_run_qual& /*qualifiers*/, fsp::drain_t drain_id, fsp::doc_id_t /*doc_id*/) override
  {
    const std::scoped_lock lock(backend_->mtx);
    backend_->existing_docs[drain_id] = backend_->existing_docs.contains(drain_id) ? backend_->existing_docs.at(drain_id) + 1 : 1;
    return true;
  }
private:
  std::shared_ptr<export_backend> backend_; // shared across every worker thread's own clone
};

// --- 4. Driving the run -----------------------------------------------------------------------

int run_export(const logger::Logger& log)
{
  auto backend = std::make_shared<export_backend>();
  // Populate backend->pending[...] here from your real data source before calling exec().

  auto cfg = fsp::exporter_config_t{
    .drain_list        = {{.id = 1, .name = "DEUTDEFF", .max_doc_txn = 100}, // NOLINT(readability-magic-numbers)
                          {.id = 2, .name = "BNPAFRPP", .max_doc_txn = 100}}, // two drains, exported concurrently
    .number_of_threads = 4,
    .filename_prefix    = "pacs008",
    .filename_ext       = "xml",
    .tmp_dir            = "/var/spool/export/tmp",
    .target_dir         = "/var/spool/export/out",
    .error_dir          = "/var/spool/export/error",
  };

  fsp::exporter<payment_txn, export_run_qual> exp(cfg, export_run_qual{.business_date = "2026-08-22"}, log, "payment-export");
  payment_export_cb                           cb(backend, log);

  auto res = exp.exec(cb);
  if (! res)
  {
    log.error("export failed: {}", res.error().to_string());
    return 1;
  }
  log.info("export done: {} document(s), {} transaction(s), {:.1f} ms", res->total_documents, res->total_transactions, res->elapsed_ms);
  return 0;
}
```

A few things worth calling out about this example:

- Two drains (`DEUTDEFF`, `BNPAFRPP`) with 4 worker threads: several threads may work the same
  drain concurrently once the other drain runs out, and a document from either drain can finish on
  any thread -- nothing here assumes a fixed drain-to-thread mapping.
- `payment_export_cb` never locks around `prepare_header()`/`prepare_transaction()`/
  `prepare_footer()` -- they only read their own by-value/by-reference arguments, not
  `backend_`, so there is nothing to guard there. Only `fetch_run_stat()`/`fetch_doc_data()`/
  `document_prepared()` touch `backend_->pending`/`backend_->existing_docs` and take the lock.
- `document_prepared()` is the natural place to commit "these rows are now exported" back to
  whatever `fetch_doc_data()` read them from, in the same critical section -- by the time it is
  called, the document is fully written to `tmp_dir`, though not yet moved to `target_dir` (that
  move happens right after, only if `document_prepared()` returns `true`).
- `fetch_doc_name()` is not overridden -- `cb_exporter<T,Q>`'s default implementation already
  builds a unique-enough name from `filename_prefix`/`filename_ext`/the run qualifiers/`drain_id`/
  block numbers.
