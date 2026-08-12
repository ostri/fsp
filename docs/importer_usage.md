# Importer usage

The importer is built to parse and validate XML documents fast on multi-core machines. Work is
split two ways at once: several documents can be processed in parallel, and a single large
document is also split into pieces (called "segments") that different threads work on at the same
time. Each worker thread cycles through three roles for whatever document it is currently handling:
Validate (check the document against an XSD schema), Cut (split the document into segments), and
Process (extract and check the actual field values from a segment).

You don't need to understand any of that machinery to use the importer, though. From a caller's
point of view, using it comes down to three steps:

1. Describe which XML elements you care about, using a small set of C++ classes (see
   [Getting your data out](#getting-your-data-out) below).
2. Call one function, `fsp::importer::exec()`, with your files and that description.
3. Read the result: either a summary of counts, or (if you want more control) a set of callback
   methods that get called live while documents are being processed.

## API description

Everything a caller needs lives in two headers: `importer.hpp` (the entry point) and
`importer_config.hpp` (the settings struct).

### `fsp::importer::exec()`

This is the *only* way to run an import -- `importer` has no public constructor, so `exec()` is
the sole entry point:

```cpp
static std::pair<std::unique_ptr<importer>, result<doc_set_counter>>
exec(const importer_config&    cfg,
     const std::vector<str_t>& xml_paths,
     cstr_t                    xsd_path,
     pipeline_hooks&           hooks = default_pipeline_hooks);
```

- `cfg` -- settings for this run (see below).
- `xml_paths` -- the list of XML files to import.
- `xsd_path` -- path to an XSD schema file, or an empty string if you don't want schema
  validation. When a schema is given, the importer checks every document against it before/while
  processing.
- `hooks` -- your own callback object (see [callback description](#callback-description)), or
  omit it entirely if you just want the summary counts.

`exec()` returns a pair:

- `std::unique_ptr<importer>` -- keep this alive as long as you need `get_results()`/
  `get_errors()`/`failed_document_indices()` (see below). It cannot be copied or moved, only held
  by this pointer.
- `result<doc_set_counter>` -- either the run's summary counts, or an error if something went
  fundamentally wrong (e.g. a file could not be opened at all). `result<T>` is a
  `std::expected<T, error_info>`, so check it like this:

```cpp
auto [importer_ptr, res] = fsp::importer::exec(cfg, xml_paths, xsd_path);
if (! res)
{
  fmt::print("Import failed: {}\n", res.error().to_string());
  return 1;
}
// res->total_docs(), res->total_segments(), res->total_segments_ok(), ... are now available
```

`doc_set_counter` (what `res` points to on success) gives you whole-run totals:

| Method                           | Meaning                                                             |
| -------------------------------- | ------------------------------------------------------------------- |
| `total_docs()`                   | number of documents in this run                                     |
| `total_segments()`               | segments processed, across all documents                            |
| `total_segments_ok()`            | of those, how many passed semantic validation                       |
| `total_segments_error()`         | of those, how many failed semantic validation                       |
| `syntactically_correct_docs()`   | documents that parsed and (if a schema was given) validated cleanly |
| `syntactically_incorrect_docs()` | documents that failed to parse or failed schema validation          |
| `semantically_correct_docs()`    | syntactically correct documents where every segment also passed     |
| `semantically_incorrect_docs()`  | syntactically correct documents with at least one failed segment    |

Once you have the `importer` object, three more methods are available on it:

- `get_results()` -- every segment that was extracted and passed semantic validation.
- `get_errors()` -- every segment that failed, either technically (bad XML/schema) or
  semantically (a field failed your own validation rule).
- `failed_document_indices()` -- indices (into `xml_paths`) of documents that failed outright.

### `importer_config`

```cpp
struct importer_config
{
                                                      // --- basic settings ---
  proc_data              targets;                     // which XML elements to extract -- see below
  std::size_t            num_of_workers = 0;          // number of worker threads
  logger::logger_config  log_config;                  // where/how to log -- see logger's own docs
  str_t                  program_name;                // shown in log lines, e.g. argv[0]
                                                      // --- intermediate settings ---
  std::size_t            ok_block_flush_size  = 1024; // see "Batch storage hooks" below
  std::size_t            nak_block_flush_size = 128;  // see "Batch storage hooks" below
                                                      // --- advanced settings ---
  std::optional<bool>    cut_with_validation;         // advanced tuning, leave as std::nullopt
  std::size_t            cutter_ratio_num = 13;       // advanced tuning, leave at default
  std::size_t            cutter_ratio_den = 6;        // advanced tuning, leave at default
  std::size_t            pool_shard_count = 2;        // advanced tuning, leave at default
};
```

For everyday use, you really only need to set three fields: `targets`, `num_of_workers`, and
`log_config` (plus `program_name`, so your log lines say who wrote them). The rest have sensible
defaults tuned from real benchmarks -- only change them if you know you need to.

`num_of_workers` is simply how many threads work on your documents at once; a good starting point
is the number of CPU cores available.

#### `cut_with_validation`

This one only matters if you gave `exec()` an XSD schema (`xsd_path`). When you did, every
document must be checked against it, and that check can happen in one of two ways:

- **Separate pass (`false`)** -- Cut (splitting the document into segments) and Validate (the XSD
  check) run as two independent passes over the document, on two separate threads, which can
  overlap in time.
- **Merged pass (`true`)** -- the XSD check is folded into the very same pass that already splits
  the document into segments. There is no separate validation pass at all.

Which one is actually faster depends on how many documents you're importing at once, based on
measurements:

- For a **single document**, cutting and validating already run in parallel on two separate
  threads, so merging them serializes work that used to overlap -- about 24% slower in one
  measurement (17.2s vs 21.3s on a 1M-transaction document).
- From **two documents up**, merging wins, and the gain grows with the document count (measured
  -14% at 2 documents, -28% at 10) -- the thread that would otherwise be dedicated to validating
  is instead free to help cut/process other documents.

Left at the default (`std::nullopt`), the importer picks whichever mode measured faster for the
actual number of documents in this run (separate for one document, merged from two up) -- you
only need to set this explicitly if your own workload doesn't match that assumption. When no XSD
schema was given (or it failed to load), this setting has no effect at all: there is nothing to
validate either way.

#### `ok_block_flush_size` / `nak_block_flush_size`

These only matter if you actually override `on_block_store()`/`on_failed_block_store()` in your own
hooks (see [Batch storage hooks](#batch-storage-hooks) below) -- with the default, do-nothing
hooks, they have no visible effect. Each worker thread accumulates the segments it processes
locally, and only calls `on_block_store()` once `ok_block_flush_size` of them have piled up (or
`on_failed_block_store()` once `nak_block_flush_size` failed ones have piled up), rather than once
per segment. Whatever is left over when a thread finishes its work is flushed in one final call,
even if it never reached the threshold. The two thresholds are separate, and `nak_block_flush_size`
defaults much lower (128 vs. 1024), because a healthy run is expected to produce far fewer failed
segments than successful ones -- without a separate, smaller threshold, failures could sit
unflushed for a very long time. Raise these if your `on_block_store()` does something with a
meaningful per-call cost (e.g. one round trip to a database) and you want fewer, larger calls;
lower them if you want results to reach your storage sooner, at the cost of more, smaller calls.

#### `cutter_ratio_num` / `cutter_ratio_den`

Advanced tuning knob for the C (cutting) to P (processing) worker-thread ratio -- leave at the
default (13:6) unless you have your own benchmark showing a different ratio works better for your
documents. The number of cutter threads is capped by document count (cutting more documents at
once than you have documents to cut is pointless), and the number of processor threads is then
derived from that cutter count using this ratio. 13:6 was found empirically fastest on a
10-document/10M-transaction benchmark, tested against 13:5, 13:7, and 13:8.

#### `pool_shard_count`

Advanced tuning knob for how many independent shards the internal segment pool splits its
ready/free queues into. More shards mean less lock contention between concurrent cutting/
processing threads, at the cost of some memory/bookkeeping overhead per shard. The default (2)
was found empirically fastest when tested against 1, 3, and 4 shards -- only change this if your
own measurements show otherwise for your workload.

### Getting your data out

`targets` tells the importer which pieces of an XML document you care about, and what to name
their fields. You describe this as ordinary C++ classes, each annotated with an XPath-like
expression, in a namespace of your own choosing:

```cpp
namespace fsp::work
{
  class [[= "header=/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"]] pacs8_header : public fsp::seg_schema
  {
  public:
    [[= "x:GrpHdr/MsgId"]]           str_t     msg_id;
    [[= "GrpHdr/NbOfTxs"]]           big_int_t no_of_txn;
    [[= "GrpHdr/TtlIntrBkSttlmAmt"]] amount_t  amount_sum;
  };
}
```

The two levels of annotation answer two different questions:

- **Class-level annotation** (`[[= "header=/x:Document/.../GrpHdr"]]` above the class) -- answers
  "how is the document cut into segments?" It marks an XPath as a segment boundary: every element
  in the document matching that path becomes one segment, and this class is the schema for that
  segment. Each class is responsible for exactly one kind of segment; a document with, say, one
  header and a thousand transactions produces one `pacs8_header` segment and a thousand
  `pacs8_txn` segments, cut out independently of each other.
- **Member-level annotation** (`[[= "x:GrpHdr/MsgId"]]` above a field) -- answers "which values
  do I want out of that segment?" It is an XPath relative to the segment's own root (not the whole
  document), and it is what actually gets extracted and handed back to you, inside a
  `fsp::segment_result&` (see [method purpose and parameter semantics](#method-purpose-and-parameter-semantics)
  below), when that segment is processed.

The importer reads this description at compile time (using C++26 reflection) and builds
everything it needs from it -- there is no separate schema file or runtime registration step.

To turn a whole namespace of these classes into the `targets` value `importer_config` expects,
call:

```cpp
cfg.targets = fsp::proc_data_of<^^fsp::work>();
```

## callback description

If you only need the summary counts (`res->total_docs()` etc.) and the full lists from
`get_results()`/`get_errors()`, you don't need a callback at all -- just omit the `hooks`
argument to `exec()`. Callbacks (`pipeline_hooks`) are for when you want to react to what's
happening *while* the import is still running: log progress, apply your own business-specific
validation rules, or stream results out to a database as they're produced instead of waiting for
the whole run to finish.

### method purpose and parameter semantics

To use callbacks, derive your own class from `fsp::pipeline_hooks_crtp<YourClass>` (not from
`pipeline_hooks` directly -- the CRTP base takes care of some bookkeeping for you) and override
whichever methods you need. Every method has a safe do-nothing default, so you only override what
you actually care about.

**Threading note:** the importer makes one independent copy of your hooks object per worker
thread (via `clone()`, handled for you by `pipeline_hooks_crtp`). Because of this, plain (not
atomic) member variables are safe to use inside `on_doc_open`/`on_doc_close`/`on_semantic_check`/
`on_block_store`/`on_failed_block_store` -- each thread only ever touches its own copy. `on_run_start()`
and `on_run_end()` are the two exceptions: they run on the main thread, on your *original* object,
not a clone, and `on_run_end()` is handed every worker clone so you can add their counters
together yourself.

| Method                    | Called                                                          | Typical use                                                     |
| ------------------------- | --------------------------------------------------------------- | --------------------------------------------------------------- |
| `on_run_start()`          | once, before anything is processed                              | start a stopwatch, log the run's document count                 |
| `on_run_end()`            | once, after every worker thread has finished                    | sum up counters from all worker clones, log a summary           |
| `get_doc_id()`            | once per document, on the main thread, before processing starts | assign your own external id to a document (e.g. a database key) |
| `on_wrk_start()`          | once per worker thread, when it starts                          | per-thread setup, e.g. open a database connection               |
| `on_wrk_end()`            | once per worker thread, when it finishes                        | per-thread cleanup, e.g. close that connection                  |
| `on_doc_open()`           | when a document starts being cut into segments                  | log which document is being processed                           |
| `on_doc_close()`          | when a document is done being cut                               | log the outcome (`fsp::doc_status`)                             |
| `on_semantic_check()`     | after a segment's fields have been extracted                    | your own semantic validation; return `true`/`false`             |
| `on_block_store()`        | periodically, for a batch of successfully validated segments    | write results out to your own storage                           |
| `on_failed_block_store()` | periodically, for a batch of failed segments                    | write/log failures out to your own storage                      |

Below is what each parameter of each method actually means -- useful once you go past the
one-line summary above.

- **`on_run_start(const doc_set_dscr& ds_dscr, const logger::Logger& log)`**
  - `ds_dscr` -- describes the whole set of documents this run was given (e.g. `ds_dscr.size()`
    for how many).
  - `log` -- this run's logger; use it instead of `std::cout`/`fmt::print` so your messages land
    in the same log file/format as the importer's own.
- **`on_run_end(const doc_set_counter& counters, const doc_set_dscr& ds_dscr, std::span<const pipeline_hooks*> worker_clones, const logger::Logger& log)`**
  - `counters` -- the same whole-run totals `exec()`'s own return value exposes (see
    [`fsp::importer::exec()`](#fspimporterexec) above).
  - `ds_dscr` -- same as `on_run_start()`'s.
  - `worker_clones` -- one pointer per worker thread, each pointing at that thread's own hooks
    clone (see the CRTP base's `clone()`). This is your only chance to see every thread's
    per-thread state (e.g. counters you accumulated in `on_semantic_check()`) all at once --
    `static_cast` each pointer back to your own hook type (it is always safe: every element was
    made by cloning your own object) and add up whatever fields you care about.
  - `log` -- same as `on_run_start()`'s.
- **`get_doc_id(std::size_t node_hint)`**
  - `node_hint` -- a document index modulo some block size meaningful to you (e.g. if you're
    generating ids with a Snowflake-style generator, this could be which "node" in that scheme
    should mint this document's id) -- deliberately not the raw document index, and never tied to
    which thread will later process the document. Ignore it if you don't need it.
  - Returns -- your own opaque, 64-bit id for this document. Whatever you return is stored on the
    document and later handed back to `on_block_store()`/`on_failed_block_store()` (via
    `doc_set_dscr`), so you can tell which document a stored segment came from.
- **`on_wrk_start(int worker_id, cstr_t thread_name, const logger::Logger& log)`** /
  **`on_wrk_end(int worker_id, cstr_t thread_name, const logger::Logger& log)`**
  - `worker_id` -- a small integer identifying this worker thread (0, 1, 2, ...).
  - `thread_name` -- this thread's own name, as it appears in log lines.
  - `log` -- same as `on_run_start()`'s.
- **`on_doc_open(std::size_t doc_ndx, const doc_dscr& dscr, const logger::Logger& log)`** /
  **`on_doc_close(std::size_t doc_ndx, doc_status status, const doc_dscr& dscr, const logger::Logger& log)`**
  - `doc_ndx` -- this document's index into the `xml_paths` vector you gave `exec()`.
  - `dscr` -- this one document's own description (e.g. `dscr.path()` for its file path).
  - `status` (`on_doc_close()` only) -- `fsp::doc_status`, this document's outcome (e.g. whether
    cutting/validation succeeded).
  - `log` -- same as `on_run_start()`'s.
- **`on_semantic_check(const xml_segment& segment, segment_result& result, bool is_first, bool is_last, const logger::Logger& log)`**
  - `segment` -- the raw cut this segment came from (offset/length/namespace/attributes of its
    top-level tag) -- only useful if you need more than the extracted values themselves.
  - `result` -- the extracted values, as a generic, name-indexed `segment_result` (see
    [`fsp::materialize_variant()`](#what-fspmaterialize_variant-does) below for turning it into
    your own schema type). Passed by non-const reference because materializing a `validated_t<X>`
    field can append to `result.errors()` if that field's own validation fails.
  - `is_first` / `is_last` -- whether this is the first/last segment of its document (both `true`
    at once for a document with exactly one segment).
  - `log` -- same as `on_run_start()`'s.
  - Returns -- this segment's semantic verdict: `true` if it's fine, `false` if something about
    its content is wrong by your own business rules.
- **`on_block_store(std::span<const std::size_t> indices, segment_pool& pool, const doc_set_dscr& ds_dscr, const logger::Logger& log)`** /
  **`on_failed_block_store(std::span<const std::size_t> indices, std::span<const error_info> errors, segment_pool& pool, const doc_set_dscr& ds_dscr, const logger::Logger& log)`**
  - `indices` -- pool slot indices belonging to this batch. Look each one up via
    `pool.segment_at(idx)`/`pool.result_at(idx)` -- these slots are guaranteed not to be reused by
    anyone else until your call returns. Can be empty (a harmless no-op call) for the final
    end-of-thread flush.
  - `errors` (`on_failed_block_store()` only) -- one entry per `indices` (same order, same
    length): why `indices[i]` failed.
  - `pool` -- the segment pool your `indices` refer to; use `pool.segment_at(idx)`/
    `pool.result_at(idx)` to read the actual segment/result data.
  - `ds_dscr` -- the full document set; look up `ds_dscr[pool.segment_at(idx).doc_ndx()]` per
    index (a batch can mix segments from different documents) to resolve that document's own
    data, e.g. the id you returned from `get_doc_id()`.
  - `log` -- same as `on_run_start()`'s.

`on_semantic_check()` is the one you'll override most often. It receives a `segment_result`, which
holds the raw, name-indexed extracted values. To get them back as your own schema type (the
classes you wrote for `targets`), call `fsp::materialize_variant<^^YourNamespace>()`:

```cpp
bool on_semantic_check(const fsp::xml_segment& segment,
                 fsp::segment_result&    result,
                 bool                    is_first,
                 bool                    is_last,
                 const logger::Logger&   log) override
{
  auto seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result);
  return std::visit([&]<typename T>(const T& s) -> bool
  {
    if constexpr (std::is_same_v<T, fsp::work::pacs8_header>)
    {
      // s.msg_id, s.amount_sum, ... are now available, fully typed
      return true; // or false, if this segment fails your own business rule
    }
    else if constexpr (std::is_same_v<T, fsp::work::pacs8_txn>)
    {
      return true;
    }
  }, seg);
}
```

The return value of `on_semantic_check()` is that segment's semantic verdict: `true` means "this
segment is fine", `false` means "something about its content is wrong, even though it parsed
correctly". This is different from a segment that fails to parse at all (a syntax error) -- that
kind of failure never reaches `on_semantic_check()` in the first place.

#### What `fsp::materialize_variant()` does

```cpp
auto seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result);
```

Breaking this one line down:

- `fsp::work` is the namespace holding your schema classes -- the same one you passed to
  `fsp::proc_data_of<^^fsp::work>()` when building `importer_config::targets` (see
  [Getting your data out](#getting-your-data-out) above). `^^fsp::work` is C++26 reflection syntax
  for "a compile-time handle to the `fsp::work` namespace itself", which is what
  `materialize_variant()` needs to know which classes to consider.
- `result.seg_type()` tells `materialize_variant()` which one of your schema classes this
  particular segment is: an integer index, in the same declaration order your classes appear in
  the namespace (the first class declared is `0`, the second is `1`, and so on). You never set
  this yourself -- it was already recorded when the segment was cut, and here you're just handing
  it back so the right class can be picked.
- `result` is the same `segment_result&` your `on_semantic_check()` override received -- the raw
  values `materialize_variant()` reads from and copies into the typed class.
- The return value, `seg`, is a `std::variant` that can hold any *one* of your schema classes --
  e.g. for the two classes in `work.hpp`'s example, it is a
  `std::variant<fsp::work::pacs8_header, fsp::work::pacs8_txn>`. Which one it actually holds
  depends on `result.seg_type()`; you don't know which at compile time, only at runtime, which is
  exactly what `std::variant` plus `std::visit()` are for -- see the `if constexpr` branches in
  the example above, one per possible schema class.

In short: `materialize_variant()` is the one call that turns the generic, name-indexed
`segment_result` you're handed into your own, fully-typed schema object (`pacs8_header` or
`pacs8_txn`, with real fields like `.msg_id`/`.amount_sum` you can read directly), so you never
have to look values up by string name yourself.

#### Batch storage hooks

`on_block_store()`/`on_failed_block_store()` exist for one specific job: writing large numbers of
results out to your own storage (a file, a database, a message queue) without doing it one
segment at a time. Instead of being called per-segment, they are called once a batch has piled up
-- controlled by `importer_config::ok_block_flush_size` (default 1024) and `nak_block_flush_size`
(default 128, since failed segments are usually much rarer) -- or once at the very end of a
worker thread's run, with whatever is left over. You receive the pool slot indices for that
batch and read the actual data back out via the `segment_pool&`/`doc_set_dscr&` parameters you're
given. Most callers don't need these two hooks at all; ignore them unless you're streaming output
to external storage while the import is still running.

### calback example

Here is a complete, minimal callback class that counts documents and segments, and applies a
simple validation rule. The callback below reads `s.amount`, so first, here is the `fsp::work`
namespace (see [Getting your data out](#getting-your-data-out) above) it materializes segments
against -- this is what defines that `pacs8_txn` even has an `amount` field to read:

```cpp
// work.hpp
#pragma once
#include "reflection.hpp"

namespace fsp::work
{
  class [[= "transaction=/Document/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
  {
  public:
    [[= "CdtTrfTxInf/PmtId/TxId"]]         str_t     txn_id;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]     amount_t  amount;
  };
}
```

Now the callback itself:

```cpp
// my_hooks.hpp
#pragma once
#include "pipeline_hooks.hpp"
#include "work.hpp"

class my_hooks : public fsp::pipeline_hooks_crtp<my_hooks>
{
public:
  std::size_t documents_seen = 0;
  std::size_t segments_ok    = 0;
  std::size_t segments_error = 0;

  void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const logger::Logger& log) override
  {
    ++documents_seen;
    log.info(fmt::format("Started document {}: '{}'", doc_ndx, dscr.path()));
  }

  bool on_semantic_check(const fsp::xml_segment& segment,
                   fsp::segment_result&    result,
                   bool                    is_first,
                   bool                    is_last,
                   const logger::Logger&   log) override
  {
    auto       seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result);
    const bool ok  = std::visit([&]<typename T>(const T& s) -> bool
    {
      if constexpr (std::is_same_v<T, fsp::work::pacs8_txn>)
        return s.amount > 0; // your own business rule
      else return true;
    }, seg);

    if (ok) ++segments_ok;
    else ++segments_error;
    return ok;
  }

  void on_run_end(const fsp::doc_set_counter&           counters,
                  const fsp::doc_set_dscr&              ds_dscr,
                  std::span<const fsp::pipeline_hooks*> worker_clones,
                  const logger::Logger&                 log) override
  {
    std::size_t total_ok = segments_ok, total_error = segments_error;
    for (const auto* clone : worker_clones)
    {
      const auto* c = static_cast<const my_hooks*>(clone); // safe: every clone IS a my_hooks
      total_ok    += c->segments_ok;
      total_error += c->segments_error;
    }
    log.info(fmt::format("Run finished: {} ok, {} failed", total_ok, total_error));
  }
};
```

Usage is the same as without a callback -- just pass your hooks object as the last argument:

```cpp
my_hooks hooks;
auto [p, res] = fsp::importer::exec(cfg, xml_paths, xsd_path, hooks);
```

For a full working example with every hook overridden, see `src/test/pacs8_cb.hpp`/
`pacs8_cb.cpp` and `src/test/pacs8-cb.cpp` in this repository.

## simple example

This is the smallest complete program that imports a set of XML files and prints a summary --
no callback needed. See `src/test/pacs8.cpp` for the full, buildable version this is based on.

```cpp
#include "importer.hpp"
#include "work.hpp" // your own target classes, see "Getting your data out" above

int main(int argc, const char* argv[])
{
  std::vector<fsp::str_t> xml_paths = { /* ... your files ... */ };
  fsp::cstr_t              xsd_path = "schema.xsd"; // or "" to skip schema validation

  auto cfg = fsp::importer_config{
    .targets        = fsp::proc_data_of<^^fsp::work>(),
    .num_of_workers = 16,
    .log_config     = logger::load_logger_config("log.conf"),
    .program_name   = "my_importer",
  };

  auto [importer_ptr, res] = fsp::importer::exec(cfg, xml_paths, xsd_path);
  if (! res)
  {
    fmt::print("Import failed: {}\n", res.error().to_string());
    return 1;
  }

  fmt::print("Documents: {}, segments ok: {}, segments failed: {}\n",
             res->total_docs(), res->total_segments_ok(), res->total_segments_error());
  return 0;
}
```

That's it: describe your target elements once (`work.hpp`), call `exec()`, check the result. Add
a callback class only once you need to react to individual documents/segments while the run is
still in progress.

## complex example

This example puts everything from this document together into one working program, based on
this repository's own `pacs8`/`pacs8-cb` demo (`src/test/work.hpp`, `pacs8_cb.hpp`/`.cpp`,
`pacs8-cb.cpp`), trimmed down to a single schema class for readability. It has all four pieces:

1. the namespace that defines how the document is cut into segments,
2. the class members that define what `on_semantic_check()` gets,
3. the callback itself, and
4. the main program tying it all together, `#include`s included.

### 1. The namespace: how the document is cut

```cpp
// work.hpp
#pragma once
#include "reflection.hpp"

namespace fsp::work
{
  // Every element matching this XPath becomes one segment, processed as a pacs8_txn.
  class [[= "transaction=/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
  {
  public:
    // clang-format off
    [[= "CdtTrfTxInf/PmtId/TxId"]]                str_t     txn_id;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt"]]             amount_t  amount;
    [[= "CdtTrfTxInf/IntrBkSttlmAmt/@Ccy"]]        o_str_t   currency;
    [[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]]   str_t     debtor_bic;
    // clang-format on
  };
} // namespace fsp::work
```

(Simplified from the real `src/test/work.hpp`: `amount` there is a `validated_t<usr::bounded_amount_t<1,
50000>>` -- a field type with its own built-in range check, applied automatically during
materialization, before `on_semantic_check()` even runs, and reported through `result.errors()` on
failure (see `result`'s own parameter description in
[method purpose and parameter semantics](#method-purpose-and-parameter-semantics) above). Plain
`amount_t` here keeps this example focused on the callback itself.)

### 2. The class members: what `on_semantic_check()` gets

The four annotated members above (`txn_id`, `amount`, `currency`, `debtor_bic`) are exactly what
ends up readable, fully typed, once you materialize a segment inside `on_semantic_check()` -- see
step 3 below, where `s.txn_id`/`s.amount` are used directly, with no string-based lookups.

### 3. The callback

```cpp
// my_pacs8_hooks.hpp
#pragma once
#include "pipeline_hooks.hpp"
#include "work.hpp"

class my_pacs8_hooks : public fsp::pipeline_hooks_crtp<my_pacs8_hooks>
{
public:
  std::size_t segments_ok    = 0;
  std::size_t segments_error = 0;

  void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr, const logger::Logger& log) override
  { log.info(fmt::format("Started document {}: '{}'", doc_ndx, dscr.path())); }

  bool on_semantic_check(const fsp::xml_segment& segment,
                         fsp::segment_result&    result,
                         bool                    is_first,
                         bool                    is_last,
                         const logger::Logger&   log) override
  {
    // Turn the generic result back into our own fsp::work::pacs8_txn -- see "What
    // fsp::materialize_variant() does" above.
    auto       seg = fsp::materialize_variant<^^fsp::work>(result.seg_type(), result);
    const bool ok  = std::visit([&]<typename T>(const T& s) -> bool
    {
      if constexpr (std::is_same_v<T, fsp::work::pacs8_txn>)
      {
        // Our own business rule: a transaction must carry a positive amount. amount_t stores its
        // value scaled by 10^amount_scale (see parsing_util.hpp), as a plain .value member.
        const bool valid = s.amount.value > 0;
        if (! valid) log.warn(fmt::format("txn {} failed validation: amount={}", s.txn_id, s.amount));
        return valid;
      }
      else return true;
    }, seg);

    if (ok) ++segments_ok;
    else ++segments_error;
    return ok;
  }

  void on_run_end(const fsp::doc_set_counter&           counters,
                  const fsp::doc_set_dscr&              ds_dscr,
                  std::span<const fsp::pipeline_hooks*> worker_clones,
                  const logger::Logger&                 log) override
  {
    std::size_t total_ok = segments_ok, total_error = segments_error;
    for (const auto* clone : worker_clones)
    {
      const auto* c = static_cast<const my_pacs8_hooks*>(clone); // safe: every clone IS a my_pacs8_hooks
      total_ok    += c->segments_ok;
      total_error += c->segments_error;
    }
    log.info(fmt::format("Run finished: {} ok, {} failed", total_ok, total_error));
  }
};
```

### 4. The main program

```cpp
// main.cpp
#include "importer.hpp"
#include "common.hpp"
#include "exe_path.hpp"
#include "my_pacs8_hooks.hpp"
#include "work.hpp" // IWYU pragma: keep
#include <fmt/format.h>
#include <iostream>
#include <logger/logger.hpp>
#include <logger/logger_config.hpp>
#include <vector>

namespace
{
  int help(fsp::str_t prog_name)
  {
    fmt::print("Usage: {0} <xml_file>* [<xsd_file>]\nExample: {0} data.xml schema.xsd\n", prog_name);
    return 1;
  }

  // log.conf (copied next to the binary by add_log_config() in CMakeLists.txt, which already
  // picked the right one of config/log.debug.json / config/log.release.json for this build's own
  // CMAKE_BUILD_TYPE at build time) is shared by every fsp program -- app_name is overwritten
  // here with this program's own name after loading it. Resolved against fsp::exe_dir() (the
  // running binary's own directory), not the caller's current working directory -- see
  // exe_path.hpp.
  [[nodiscard]] logger::logger_config load_program_logger_config(fsp::cstr_t program_name)
  {
    const auto config_path = fsp::exe_dir() / "log.conf";
    auto       cfg         = logger::load_logger_config(config_path.string());
    cfg.app_name           = program_name;
    return cfg;
  }
} // namespace

int main(int argc, const char* argv[])
{
  fsp::param args = fsp::load_args(argc, argv); // collects xml_file(s)/xsd_file/program name from argv
  if (args.files.empty()) return help(args.p_name);

  try
  {
    auto cfg = fsp::importer_config{
      .targets        = fsp::proc_data_of<^^fsp::work>(), // built from step 1's namespace
      .num_of_workers = 16,
      .log_config     = load_program_logger_config(args.p_name),
      .program_name   = args.p_name,
    };

    my_pacs8_hooks hooks; // step 3's callback
    auto [importer_ptr, res] = fsp::importer::exec(cfg, args.files, args.xsd_file, hooks);
    if (! res)
    {
      fmt::print("Processing failed: '{}'\n", res.error().to_string());
      return 1;
    }

    fmt::print("Documents: {}, segments ok: {}, segments failed: {}\n",
               res->total_docs(), res->total_segments_ok(), res->total_segments_error());
  }
  catch (const std::exception& e)
  {
    std::cerr << "Fatal error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
```

Compare this against the real, buildable version this is based on: `src/test/work.hpp` (the full
schema, with a second class for the message header), `src/test/pacs8_cb.hpp`/`pacs8_cb.cpp` (the
full callback, with every hook overridden), and `src/test/pacs8-cb.cpp` (the main program).
