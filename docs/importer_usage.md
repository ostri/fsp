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
   [Getting your data out](#13-getting-your-data-out) below).
2. Call one function, `fsp::importer::exec()`, with your files and that description.
3. Read the result: either a summary of counts, or (if you want more control) a set of callback
   methods that get called live while documents are being processed.

## Table of contents

1. [API description](#1-api-description)
   1. [`fsp::importer::exec()`](#11-fspimporterexec)
   2. [`importer_config`](#12-importer_config)
      1. [`cut_with_validation`](#121-cut_with_validation)
      2. [`ok_block_flush_size` / `nak_block_flush_size`](#122-ok_block_flush_size--nak_block_flush_size)
      3. [`cutter_ratio_num` / `cutter_ratio_den`](#123-cutter_ratio_num--cutter_ratio_den)
      4. [`pool_shard_count`](#124-pool_shard_count)
   3. [Getting your data out](#13-getting-your-data-out)
      1. [Attribute paths and field types](#131-attribute-paths-and-field-types)
      2. [Marking a schema class as a header segment](#132-marking-a-schema-class-as-a-header-segment)
2. [Callback description](#2-callback-description)
   1. [Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics)
   2. [Document lifecycle, in order](#22-document-lifecycle-in-order)
   3. [Segment processing order](#23-segment-processing-order)
      1. [Header segments are processed first](#231-header-segments-are-processed-first)
      2. [Knowing every segment has been stored, before `on_doc_close()`](#232-knowing-every-segment-has-been-stored-before-on_doc_close)
      3. [Batch storage hooks](#233-batch-storage-hooks)
   4. [Callback interface](#24-callback-interface)
   5. [Run-level and document-level shared data](#25-run-level-and-document-level-shared-data)
   6. [Callback example](#26-callback-example)
3. [Simple example](#3-simple-example)
4. [Complex example](#4-complex-example)
   1. [The namespace: how the document is cut](#41-the-namespace-how-the-document-is-cut)
   2. [The class members: what `on_type()` gets](#42-the-class-members-what-on_type-gets)
   3. [The callback](#43-the-callback)
   4. [The main program](#44-the-main-program)
5. [A note on the class hierarchy](#5-a-note-on-the-class-hierarchy)
6. [Document errors](#6-document-errors)
   - [What is implemented vs. what remains a proposal](#60-what-is-implemented-vs-what-remains-a-proposal)
   1. [Error classes](#61-error-classes)
   2. [Error detection](#62-error-detection)
   3. [Error notification](#63-error-notification)
   4. [Error cleanup -- cached data](#64-error-cleanup----cached-data)
   5. [Error cleanup -- permanent storage](#65-error-cleanup----permanent-storage)
   6. [Error cleanup -- ongoing](#66-error-cleanup----ongoing)
7. [Internals](#7-internals)
   1. [Waiting queues](#71-waiting-queues)
      1. [The instruction queue (design proposal -- not yet implemented)](#711-the-instruction-queue-design-proposal----not-yet-implemented)
      2. [Queues that exist today](#712-queues-that-exist-today)
   2. [Document related structures](#72-document-related-structures)
      1. [`doc_set_dscr`](#721-doc_set_dscr)
      2. [`doc_dscr`](#722-doc_dscr)
      3. [`doc_status_t`](#723-doc_status_t)

## 1. API description

Everything a caller needs lives in two headers: `importer.hpp` (the entry point) and
`importer_config.hpp` (the settings struct).

### 1.1. `fsp::importer::exec()`

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
- `hooks` -- your own callback object (see [callback description](#2-callback-description)), or
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

### 1.2. `importer_config`

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

#### 1.2.1. `cut_with_validation`

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

#### 1.2.2. `ok_block_flush_size` / `nak_block_flush_size`

These only matter if you actually override `on_block_store()`/`on_failed_block_store()`
in your own hooks (see [Batch storage hooks](#233-batch-storage-hooks) below) -- with the default,
do-nothing hooks, they have no visible effect. Each worker thread accumulates the segments it
processes locally, and only calls `on_block_store()` once `ok_block_flush_size` of them have
piled up (or `on_failed_block_store()` once `nak_block_flush_size` failed ones have piled
up), rather than once per segment. Whatever is left over when a thread finishes its work is
flushed in one final call, even if it never reached the threshold. The two thresholds are
separate, and `nak_block_flush_size` defaults much lower (128 vs. 1024), because a healthy run is
expected to produce far fewer failed segments than successful ones -- without a separate, smaller
threshold, failures could sit unflushed for a very long time. Raise these if your
`on_block_store()` does something with a meaningful per-call cost (e.g. one round trip to a
database) and you want fewer, larger calls; lower them if you want results to reach your storage
sooner, at the cost of more, smaller calls.

#### 1.2.3. `cutter_ratio_num` / `cutter_ratio_den`

Advanced tuning knob for the C (cutting) to P (processing) worker-thread ratio -- leave at the
default (13:6) unless you have your own benchmark showing a different ratio works better for your
documents. The number of cutter threads is capped by document count (cutting more documents at
once than you have documents to cut is pointless), and the number of processor threads is then
derived from that cutter count using this ratio. 13:6 was found empirically fastest on a
10-document/10M-transaction benchmark, tested against 13:5, 13:7, and 13:8.

#### 1.2.4. `pool_shard_count`

Advanced tuning knob for how many independent shards the internal segment pool splits its
ready/free queues into. More shards mean less lock contention between concurrent cutting/
processing threads, at the cost of some memory/bookkeeping overhead per shard. The default (2)
was found empirically fastest when tested against 1, 3, and 4 shards -- only change this if your
own measurements show otherwise for your workload.

### 1.3. Getting your data out

`targets` tells the importer which pieces of an XML document you care about, and what to name
their fields. You describe this as ordinary C++ classes, each annotated with an XPath-like
expression, in a namespace of your own choosing:

```cpp
namespace fsp::work
{
  class [[= "/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"]] pacs8_hdr : public fsp::hdr_seg_schema
  {
  public:
    [[= "x:GrpHdr/MsgId"]]           str_t     msg_id;
    [[= "GrpHdr/NbOfTxs"]]           big_int_t no_of_txn;
    [[= "GrpHdr/TtlIntrBkSttlmAmt"]] amount_t  amount_sum;
  };
}
```

`fsp::work` here is just this example's own choice of name -- yours can be called anything. It is
the namespace where you define what a segment *is* (which schema classes exist, i.e. which kinds
of segments the document gets cut into) and what values you want pulled out of each one (the
annotated members below); `fsp::proc_data_of<^^fsp::work>()` (see the end of this section) reads
that description and turns it into the `targets` value `importer_config` expects.

The two levels of annotation answer two different questions:

- **Class-level annotation** (`[[= "/x:Document/.../GrpHdr"]]` above the class) -- answers
  "how is the document cut into segments?" It marks an XPath as a segment boundary: every element
  in the document matching that path becomes one segment, and this class is the schema for that
  segment. Each class is responsible for exactly one kind of segment; a document with, say, one
  header and a thousand transactions produces one `pacs8_hdr` segment and a thousand
  `pacs8_txn` segments, cut out independently of each other. The segment's own name (e.g. for log
  lines, see `on_doc_open()`'s log output) is simply the class's own C++ identifier -- `pacs8_hdr`,
  `pacs8_txn` -- there is nothing else to name separately.
- **Member-level annotation** (`[[= "x:GrpHdr/MsgId"]]` above a field) -- answers "which values
  do I want out of that segment?" It is an XPath relative to the segment's own root (not the whole
  document), and it is what actually gets extracted and handed back to you, inside a
  `fsp::segment_result&` (see [Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics)
  below), when that segment is processed.
- **Base class** (`public fsp::seg_schema` vs. `public fsp::hdr_seg_schema` above `pacs8_hdr`) --
  answers "is this a header segment?" See
  [Marking a schema class as a header segment](#132-marking-a-schema-class-as-a-header-segment)
  below for what that distinction actually does.

Any `prefix:` you use inside these paths (e.g. the `x:` in `x:GrpHdr/MsgId`) is resolved against
a prefix -> namespace URI table your own namespace must declare, named exactly `ns`:

```cpp
namespace fsp::work
{
  static constexpr auto ns = std::to_array<fsp::ns>({
    {.prefix = "",  .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // default namespace
    {.prefix = "x", .uri = "urn:iso:std:iso:20022:tech:xsd:pacs.008.001.08"}, // explicit prefix
  });
  // ... schema classes below
}
```

An unprefixed path segment (e.g. `GrpHdr/NbOfTxs` above) resolves against whichever entry has
`.prefix = ""` (the document's default namespace) -- it does NOT mean "any/no namespace". A path
segment can freely mix prefixed and unprefixed steps (`x:GrpHdr/MsgId` vs. `GrpHdr/NbOfTxs` above
both resolve to the same URI here, since `""` and `"x"` happen to share one). `proc_data_of()`
(see below) looks this table up by name via reflection -- it is a compile-time error
("namespace is missing a 'ns' member") if your namespace doesn't declare one.

The importer reads this description at compile time (using C++26 reflection) and builds
everything it needs from it -- there is no separate schema file or runtime registration step.

To turn a whole namespace of these classes into the `targets` value `importer_config` expects,
call:

```cpp
cfg.targets = fsp::proc_data_of<^^fsp::work>();
```

#### 1.3.1. Attribute paths and field types

**`@` marks an XML attribute, not a child element** -- e.g.
`[[= "GrpHdr/TtlIntrBkSttlmAmt/@Ccy"]]` reads the `Ccy` *attribute* of the `TtlIntrBkSttlmAmt`
element, not a child element named `Ccy`. It can carry a prefix too (`.../@x:Ccy`), resolved
against the same `ns` table as everything else. `@` is only meaningful as the last step of a path
-- it marks which XML node in the path carries the attribute, not a navigation step of its own.

The field's own declared C++ type (not a marker character in the path string) is what tells the
importer both *what scalar kind* of value to parse the extracted text as, and *how many* of them
to expect. The rest of this section goes through each type family in turn.

##### 1.3.1.1. Scalar base types

These are the seven C++ types `convert_scalar()` (see `reflection.hpp`) knows how to parse XML
text into -- every other type family below (`o_`, `m_`, `validated_t<X>`) is built out of one of
these seven, or out of your own `validated_t<X>` payload type. Declare a field with one of these
directly when exactly one occurrence is required and no extra validation is needed -- a missing
occurrence is then a technical (not semantic) failure.

| Type          | Declare a field as...     | Parsed from                                                        |
| ------------- | ------------------------- | ------------------------------------------------------------------ |
| `str_t`       | `str_t msg_id;`           | copied through as-is (`std::string`)                               |
| `big_int_t`   | `big_int_t no_of_txn;`    | an integer, via `std::from_chars` (`std::uint64_t`)                |
| `int_t`       | `int_t some_field;`       | an integer, via `std::from_chars` (`std::int32_t`)                 |
| `small_int_t` | `small_int_t some_field;` | an integer, via `std::from_chars` (`std::int16_t`)                 |
| `date_t`      | `date_t value_date;`      | an ISO 20022 `ISODate` (`YYYY-MM-DD`), via `parse_iso_date()`      |
| `ts_t`        | `ts_t msg_ts;`            | an ISO 20022 `ISODateTime`, via `parse_iso_datetime()`             |
| `amount_t`    | `amount_t amount_sum;`    | an ISO 20022 decimal amount, via `parse_iso_amount()` -- see below |

**`amount_t`** deserves its own note: it is a thin wrapper (`struct amount_t { big_int_t value; }`),
deliberately a distinct C++ type from `big_int_t` rather than a plain alias, so `convert_scalar()`
can tell the two apart and parse `amount_t` as a *decimal* (e.g. `"123.45"`) instead of a plain
integer. Internally it stores the value scaled by `10^amount_scale` (`amount_scale = 5`, e.g.
`"123.45"` becomes the raw integer `12345000`) -- read it back as money via its own
`fmt::formatter` (e.g. `fmt::format("{}", hdr.amount_sum)` prints `"123.45000"`), not by reading
`.value` directly.

##### 1.3.1.2. `o_` prefix -- optional (0 or 1 occurrences)

One `o_`-prefixed alias per scalar base type above, each an alias for `std::optional<T>`:
`o_str_t`, `o_big_int_t`, `o_int_t`, `o_small_int_t`, `o_date_t`, `o_ts_t`, `o_amount_t` (all
declared in `parsing_util.hpp`). Use one of these when the XML element/attribute may legitimately
be absent -- a missing occurrence is fine, and the field materializes as `std::nullopt`, not a
failure:

```cpp
[[= "GrpHdr/TtlIntrBkSttlmAmt/@Ccy"]] o_str_t currency; // not every message repeats the currency
```

##### 1.3.1.3. `m_` prefix -- repeated, fixed capacity (0 to `max_values` occurrences)

One `m_`-prefixed alias per scalar base type above, each an alias for `std::array<T, max_values>`
(`max_values = 10`, see `parsing_util.hpp`): `m_str_t`, `m_big_int_t`, `m_int_t`, `m_small_int_t`,
`m_date_t`, `m_ts_t`, `m_amount_t`. Use one of these when the same path can legitimately match
more than once within a segment; each occurrence is extracted independently, in order, into
successive array slots -- slots beyond the number actually found keep their default-constructed
value, and occurrences beyond `max_values` are silently dropped (fixed capacity trades unbounded
growth for a predictable, allocation-free field):

```cpp
[[= "CdtTrfTxInf/InstgAgt/FinInstnId/BICFI"]] m_str_t instr_agent; // a transaction can name more than one instructing agent
```

A plain `std::vector<T>` field (no `m_` alias -- just write `std::vector<T>` yourself) works the
same way but grows without the `max_values` cap, at the cost of a heap allocation; prefer `m_*`
unless you specifically expect to exceed `max_values` occurrences.

##### 1.3.1.4. `validated_t<X>` -- a field that validates itself

`validated_t<X>` (an alias for `std::expected<X, int>`, see `reflection.hpp`) wraps a field type
`X` that validates itself: `X` must provide `static std::expected<X, error_info> parse(cstr_t)`,
called automatically during extraction instead of a plain string-to-value conversion. On success
the field holds `X`; on failure it holds an index into the owning `segment_result::errors()` (see
that parameter's own description in
[Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics) above) -- this
is what lets a field validate itself (an IBAN's checksum, an amount's allowed range, a BIC against
a reference table, ...) entirely inside the schema class, with no extra code in your callback:

```cpp
[[= "CdtTrfTxInf/DbtrAcct/Id/IBAN"]] validated_t<fsp::ach::iban_t> debtor_iban;
```

Three examples of a `validated_t<X>` payload type, all from this repository:

- **`fsp::ach::iban_t`** (`ach/utility.hpp`) -- validates that a string is a well-formed IBAN
  (checksum included). Stateless: every `parse()` call is independent, nothing to load ahead of
  time.
- **`usr::bounded_amount_t<Min, Max>`** (`src/test/user_types.hpp`) -- validates that an
  `amount_t` falls within `[Min, Max]` (given in whole currency units, e.g.
  `bounded_amount_t<1, 50000>`). `Min`/`Max` are compile-time template parameters -- the allowed
  range is fixed in the schema declaration itself, nothing to load at runtime either.
- **`fsp::value_set_t<Tag>`** (`value_set_t.hpp`) -- validates that a string is a member of a
  fixed set of allowed values, loaded once at runtime rather than known at compile time (e.g. a
  reference table of valid BIC codes, too large/data-driven to spell out as template parameters).
  See [Membership validation with `value_set_t`](#1315-membership-validation-with-value_set_t) below for
  how to set one up.

`validated_t<X>` combines freely with the `o_`/`m_` cardinality prefixes too --
**`o_validated_t<X>`**/**`m_validated_t<X>`** (both in `reflection.hpp`) are the optional/repeated
counterparts of plain `validated_t<X>`, same relationship as `o_str_t`/`m_str_t` are to `str_t`:

```cpp
[[= "SomePath/OptionalValidatedField"]] o_validated_t<fsp::ach::iban_t> maybe_iban;
[[= "SomePath/RepeatedValidatedField"]] m_validated_t<fsp::ach::iban_t> several_ibans;
```

##### 1.3.1.5. Membership validation with `value_set_t`

`fsp::value_set_t<Tag>` (`value_set_t.hpp`) is a `validated_t<X>` payload type for the common case
of "must be one of these values", where the set of allowed values is data (a reference table),
not something you'd want to hand-write as a C++ literal or spell out per-value in template
parameters. One `value_set_t<Tag>` instantiation covers both cardinalities of that problem:

- **A single fixed value** (e.g. a scheme code that must always read exactly `"SEPA"`) --
  `init()` with a one-element set.
- **Membership in a reference table** (e.g. a BIC code that must appear in a table of known
  agents) -- `init()` with the whole table.

`Tag` is never constructed or referenced by value -- it exists purely so two independent sets
(e.g. BIC codes and currency codes) don't collide on the same underlying storage; declare an empty
tag struct and a named alias next to it:

```cpp
// user_types.hpp
struct bic_codes_tag {};
using bic_code_t = fsp::value_set_t<bic_codes_tag>;
```

```cpp
// work.hpp
[[= "CdtTrfTxInf/DbtrAgt/FinInstnId/BICFI"]] validated_t<usr::bic_code_t> debtor_bic;
```

Before any segment is processed, populate the set from your own delimiter-separated buffer, via
`bic_code_t::init(packed_values, delimiter)` -- typically from your own `pipeline_hooks::on_run_init()`
or `on_run_start()` override (both guaranteed to run on the main thread, strictly before any worker
thread starts, see [Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics)
above). Prefer `on_run_init()` if the setup can fail (e.g. it reads a database) -- returning an
error from it stops the run before `on_run_start()`/any document is ever cut:

```cpp
fsp::e_void my_hooks::on_run_start(const fsp::doc_set_dscr& ds_dscr)
{
  usr::bic_code_t::init("HAABSI22;BAKOSI2X;KSPKSI22", ';'); // one call, before any parse()
  return {};
}
```

`init()` does NOT copy `packed_values` -- it stores `string_view`s into your own buffer, so that
buffer must stay alive and unmodified for as long as any `parse()` call for this `Tag` can still
happen, i.e. for the whole run. Every `parse()` call after `init()` -- there can be millions, one
per matching field across every worker thread -- is then just an O(1) hash-set lookup, never
touching your raw data again.

#### 1.3.2. Marking a schema class as a header segment

`fsp::seg_schema` (the marker base every schema class derives from, see
[Getting your data out](#13-getting-your-data-out) above) declares one method beyond being a plain
marker: `static consteval bool is_header()`, which it defines to always return `false`.
`fsp::hdr_seg_schema` is a second marker base, itself derived from `fsp::seg_schema`, that
overrides `is_header()` to return `true` instead -- deriving a schema class from `hdr_seg_schema`
in place of plain `seg_schema` is the whole mechanism for marking that class's segments as headers:

```cpp
// A plain segment -- seg_schema::is_header() answers false for it.
class [[= "/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema { /* ... */ };

// A header segment -- hdr_seg_schema::is_header() answers true for it instead.
class [[= "/x:Document/FIToFICstmrCdtTrf/x:GrpHdr"]] pacs8_hdr : public fsp::hdr_seg_schema { /* ... */ };
```

`hdr_seg_schema` adds no data members of its own -- it exists purely to carry a different
`is_header()` answer, so switching a class from `seg_schema` to `hdr_seg_schema` (or back) has no
effect on that class's own size/layout, and a `hdr_seg_schema`-derived class still satisfies every
`std::is_base_of_v<seg_schema, T>` check `classes_of()`/`proc_data_of()` run internally (since
`hdr_seg_schema` itself derives from `seg_schema`) -- it is picked up as an ordinary schema class in
every respect except this one flag.

`is_header()` being `static consteval` (not a virtual, not an instance method) means the answer is
a property of the schema class itself, known entirely at compile time -- there is nothing to
construct, and no way for it to differ between two segments of the same schema class.
`proc_data_of<^^YourNamespace>()` (see above) reads every schema class's own `is_header()` while it
walks your namespace and records the answers into `proc_data::is_header`, a `std::vector<bool>`
indexed the same way `proc_data::xpaths` already is (declaration order, i.e. the same index
`segment_result::seg_type()` returns for a processed segment) -- this is what `cfg.targets` (built
by that same call) carries into `exec()`, with nothing left for you to configure separately.
Marking one or more
schema classes with `hdr_seg_schema` enables the header-priority queue described in
[Header segments are processed first](#231-header-segments-are-processed-first) below; a document
format with no real header concept (or one where processing order genuinely doesn't matter) simply
never uses `hdr_seg_schema` at all -- every schema class stays a plain `seg_schema`, and that whole
mechanism is a no-op.

## 2. Callback description

If you only need the summary counts (`res->total_docs()` etc.) and the full lists from
`get_results()`/`get_errors()`, you don't need a callback at all -- just omit the `hooks`
argument to `exec()`. Callbacks (`pipeline_hooks`) are for when you want to react to what's
happening *while* the import is still running: log progress, apply your own business-specific
validation rules, or stream results out to a database as they're produced instead of waiting for
the whole run to finish.

### 2.1. Method purpose and parameter semantics

To use callbacks, derive your own class from `fsp::typed_semantic_check<YourClass, ^^YourNamespace>`
(not from `pipeline_hooks` directly -- this base takes care of some bookkeeping for you, see
[A note on the class hierarchy](#5-a-note-on-the-class-hierarchy) below) and override whichever
methods you need -- `on_run_start()`, `on_wrk_start()`, `on_doc_open()`, `on_type()`, and so on.
Every one of these has a safe do-nothing default (or, for `on_type()`, a sensible default
verdict), so you only override what you actually care about.

**Logging:** call the protected `log()` accessor from inside any hook to get this run's logger --
use it instead of `std::cout`/`fmt::print` so your messages land in the same log file/format as
the importer's own.

**Threading note:** the importer makes one independent copy of your hooks object per worker
thread (via `clone()`, handled for you by `pipeline_hooks_crtp`, which `typed_semantic_check`
derives from). Because of this, plain (not atomic) member variables are safe to use inside
`on_doc_open`/`on_doc_cutting_end`/`on_type`/`on_doc_sem_check`/`on_doc_stored`/`on_doc_close`/
`on_doc_finish`/`on_block_store`/`on_failed_block_store` -- each thread only ever touches its own
copy. `on_run_start()` and `on_run_end()` are the two exceptions: they run on the main thread, on
your *original* object, not a clone, and `on_run_end()` is handed every worker clone so you can add
their counters together yourself.

### 2.2. Document lifecycle, in order

This is the part that matters for knowing, at any moment, exactly where a document stands: when it
was opened, when it was fully cut into segments, when each segment (including the header) was
checked, when every segment has actually been written to storage, and when the document is
finally, truly closed. Seven hooks fire, in this order, for every document (a segment that fails to
even parse skips `on_type()` and goes straight to `record_segment_failed()`'s own bookkeeping
instead, but still counts towards "all segments processed"/"all segments stored" below):

1. **`on_doc_open(doc_ndx, dscr)`** -- the document has been handed to a cutter thread; cutting is
   about to start.
2. **`on_doc_cutting_end(doc_ndx, dscr)`** -- the document has been fully split into segments.
   This does **not** mean syntax/validation/semantics are known yet -- only that the cutter is done
   producing segments (renamed from the old, misleadingly-named `on_doc_close()` this hook used to
   be -- see point 6 below for the hook that now actually fires once the document is closed).
3. **`on_type()`** (dispatched from `on_seg_sem_check()`) -- fires once per segment, in whatever
   order/thread each segment happens to be processed, including the document's header segment like
   any other. Use `is_first`/`is_last` if you need to know "this was the header" / "this was the
   last segment" without tracking it yourself.
4. **`on_doc_sem_check(doc_ndx)`** -- fires exactly once, as soon as every one of the document's
   segments has gone through step 3 (fsp-core's own "all segments processed" condition) --
   independent of whether syntax/validation are known yet. This is your one chance to check
   document-wide invariants across segments (e.g. "declared count in the header matches the actual
   number of transaction segments") via `doc_data(doc_ndx)`. Returns this document's own semantic
   verdict.
5. **`on_doc_stored(doc_ndx, dscr)`** -- fires exactly once, as soon as every one of the document's
   segments has actually been written out via `on_block_store()`/`on_failed_block_store()` (see
   [Batch storage hooks](#233-batch-storage-hooks) below) -- independent of steps 3/4 above and of
   syntax/validation. Because a single P-role worker thread can be mid-flight on segments from
   several different documents at once, and a flush batch can freely mix segments from different
   documents, this is NOT simply "one flush happened" -- it is fsp-core counting, per document, how
   many of its segments have actually left `on_block_store()`/`on_failed_block_store()`, against the
   same total the cutter already recorded for step 4. This is your signal that nothing more will
   ever be written for this document.
6. **`on_doc_close(doc_ndx, verdict, err, dscr, segments_stored)`** -- fires exactly once, as soon
   as syntax, validation, step 4's semantic verdict, AND step 5's storage-completeness are ALL known
   (whichever of those four facts happens to complete last is what triggers it -- there is no fixed
   order between steps 3/4 and step 5, only that step 5 is now, by construction, always known
   before this fires). `verdict.is_finished()` is guaranteed true here; `verdict.ok()` gives the
   final aggregate, `verdict.syntax_status()`/`valid_status()`/`semantic_status()` give the
   individual partial results if you need to know exactly which fact failed. `segments_stored` is
   this document's own final `doc_counters::stored_count()` tally (`0` for a document rejected
   before any segment was ever processed).
7. **`on_doc_finish(doc_ndx)`** -- fires immediately after `on_doc_close()` returns. `doc_data
   (doc_ndx)`'s timing is already stopped by the time this runs, so `duration()` is safe to read.
   This is the LAST point at which `doc_data(doc_ndx)` is guaranteed to still hold this document's
   own state -- right after this call returns, the slot may be reset and handed to a different
   document.

### 2.3. Segment processing order

Once a document is cut, its segments land in a shared, sharded ready queue that every P-role
worker thread pulls from -- across every document currently being processed, not just one. This
section covers two things that follow from that: how header segments are kept from starving behind
a pile of ordinary ones, and how fsp-core knows every one of a document's segments has actually
been written to storage before it lets `on_doc_close()` fire.

#### 2.3.1. Header segments are processed first

Most document formats (SEPA pain/pacs messages included) have one or more header segments (e.g.
`GrpHdr`) that logically precede a much larger number of transaction segments -- and it's common
for a cb's own business logic to need the header's data (via `doc_data(doc_ndx)`) before it can
meaningfully validate a transaction (see [Run-level and document-level shared
data](#25-run-level-and-document-level-shared-data) below for the `doc_data()` mechanism itself). If
header and transaction segments sat in the same ready queue with no distinction, every P-role
thread could end up busy on transaction segments -- from this document or others -- while the one
header segment that would unblock them all sits further back in the queue, never picked up because
nothing prioritizes it.

fsp-core avoids that with a second, parallel set of ready queues reserved for header segments only
(sharded the same way as the ordinary ready queues, for the same contention reasons). A cutter
routes each segment into the header queue or the ordinary queue as it produces it, based on whether
that segment's own schema class derives from `fsp::hdr_seg_schema` rather than plain
`fsp::seg_schema` (see
[Marking a schema class as a header segment](#132-marking-a-schema-class-as-a-header-segment)
above) -- if your namespace declares no `hdr_seg_schema`-derived class at all, every segment goes
into the ordinary queue and this whole mechanism is a no-op. Every P-role worker thread, each time
it looks for work, checks the header queues (its own shard, then a sweep
of the others) *before* it ever looks at the ordinary ready queues; only once no header segment is
waiting anywhere does it fall through to ordinary segments, exactly as today. Because this check
happens on every single work-fetch, not just at thread start-up, a header segment can never be
starved behind an unbounded pile of transaction segments -- the moment one becomes ready, the next
thread that goes looking for work picks it up first, regardless of which document it belongs to or
how much ordinary work is already queued.

In the common case this costs nothing: a document's header segment is produced by the cutter
before its transaction segments (SEPA headers precede transactions in document order), so the
header queue is usually already empty again by the time transaction segments start arriving. The
mechanism exists as a guarantee for the uncommon case, not as a reordering of the common one.

#### 2.3.2. Knowing every segment has been stored, before `on_doc_close()`

`on_doc_stored()` (see step 5 in [Document lifecycle, in order](#22-document-lifecycle-in-order)
above) is fsp-core's answer to "has everything this document produced actually left `on_block_
store()`/`on_failed_block_store()` yet" -- and the reason it needs its own mechanism, rather than
just counting flushes, is the same sharing that makes the header-queue problem possible in the
first place: any P-role thread can process segments from any document, a single flush batch can
freely mix segments from several different documents (see
[Batch storage hooks](#233-batch-storage-hooks) above), and one document's segments can be spread
across several batches, flushed by several different worker threads, at different times. Counting
"batches flushed" would tell you
nothing about any one document.

Instead, fsp-core counts **segments actually written**, per document, against a total it already
has on hand: the cutter records each document's exact segment count the moment cutting finishes
(the same total `on_doc_sem_check()`'s own "all segments processed" condition already compares
against). Every time `on_block_store()`/`on_failed_block_store()` is about to fire for a batch,
fsp-core groups that batch's indices by their own `doc_ndx` first (a batch can span several
documents, as above) and, for each document represented in it, adds however many of its segments
just left the batch to that document's own running "segments stored" count. The moment that count
reaches the document's known total, `on_doc_stored()` fires for that document -- exactly once,
regardless of which thread's flush happened to be the one that pushed the count over the line.

`on_doc_stored()`'s completion is then folded into the SAME four-way completion gate `on_doc_
close()` already waits on (syntax, validation, `on_doc_sem_check()`'s semantic verdict, and now
storage-completeness) -- so `on_doc_close()` is structurally unable to fire until every one of a
document's segments has actually left `on_block_store()`/`on_failed_block_store()`, no matter which
of the four facts happens to become true last. You never need to track or count anything yourself
to get this guarantee.

#### 2.3.3. Batch storage hooks

`on_block_store()`/`on_failed_block_store()` exist for one specific job: writing large
numbers of results out to your own storage (a file, a database, a message queue) without doing it
one segment at a time. Instead of being called per-segment, they are called once a batch has
piled up -- controlled by `importer_config::ok_block_flush_size` (default 1024) and
`nak_block_flush_size` (default 128, since failed segments are usually much rarer) -- or once at
the very end of a worker thread's run, with whatever is left over. You receive the pool slot
indices for that batch and read the actual data back out via the `segment_pool&`/`doc_set_dscr&`
parameters you're given. Most callers don't need these two hooks at all; ignore them unless you're
streaming output to external storage while the import is still running.

A single batch can freely mix segments belonging to different documents (a P-role worker thread
processes whatever segment comes next, regardless of which document it belongs to), and one
document's segments can be spread across several batches, even across several worker threads.
fsp-core tracks, per document, how many of its segments have actually left one of these two hooks,
against the same total the cutter already knows -- once that count reaches the total, `on_doc_
stored()` (see [Document lifecycle, in order](#22-document-lifecycle-in-order) above) fires for that
document, exactly once. You never need to do this counting yourself.

### 2.4. Callback interface

The full catalog of `pipeline_hooks` override points -- every method a `typed_semantic_check`-
derived callback can override, in one place. The "when" step numbers below (1 through 7) match
[Document lifecycle, in order](#22-document-lifecycle-in-order) above; hooks with no step number
either run once per run/worker/document-id rather than as part of that per-document sequence, or
are the batch storage hooks covered separately in
[Batch storage hooks](#233-batch-storage-hooks) above.

- **`[[nodiscard]] e_void on_run_init()`**
  - when: once, before `on_run_start()`, can fail
  - usage: fallible one-time setup a package-level cb owns (e.g. read a database into `run_data()`); an error here skips `on_run_start()` and stops the run before any document is cut
  - parameters: *(none)*
- **`[[nodiscard]] e_void on_run_start(const doc_set_dscr& ds_dscr)`**
  - when: once, before anything is processed (after `on_run_init()` succeeds)
  - usage: start a stopwatch, log the run's document count
  - parameters:
    - `ds_dscr` : `const doc_set_dscr&` -- describes the whole set of documents this run was given (e.g. `ds_dscr.size()` for how many)
- **`e_void on_run_end(const doc_set_counter& counters, const doc_set_dscr& ds_dscr, std::span<const pipeline_hooks*> worker_clones)`**
  - when: once, after every worker thread has finished
  - usage: sum up per-thread state accumulated in `worker_clones`, log a summary
  - parameters:
    - `counters` : `const doc_set_counter&` -- the same whole-run totals `exec()`'s own return value exposes (see [`fsp::importer::exec()`](#11-fspimporterexec) above)
    - `ds_dscr` : `const doc_set_dscr&` -- same as `on_run_start()`'s
    - `worker_clones` : `std::span<const pipeline_hooks*>` -- one pointer per worker thread, each pointing at that thread's own hooks clone; `static_cast` each back to your own hook type (always safe: every element was made by cloning your own object) and add up whatever fields you care about
- **`[[nodiscard]] std::uint64_t get_doc_id(std::size_t node_hint)`**
  - when: once per document, on the main thread, before processing starts
  - usage: assign your own external id to a document (e.g. a database key)
  - parameters:
    - `node_hint` : `std::size_t` -- a document index modulo some block size meaningful to you (e.g. which "node" a Snowflake-style generator should mint this document's id from) -- deliberately not the raw document index, never tied to which thread later processes the document; ignore it if you don't need it
  - returns: your own opaque, 64-bit id for this document -- stored on the document, later handed back to `on_block_store()`/`on_failed_block_store()` via `doc_set_dscr`
- **`[[nodiscard]] std::optional<std::int16_t> get_doc_agent_id(cstr_t path)`**
  - when: once per document, right after `get_doc_id()`, on the main thread
  - usage: resolve an opaque per-document id (e.g. from the file name) once, up front
  - parameters:
    - `path` : `cstr_t` -- the document's own path, exactly as passed to `exec()`
  - returns: your own opaque id; stored as `doc_dscr::agent_id()`, readable back from any later hook that gets a `doc_dscr`. `0` is fsp-core's own "unresolved agent" convention -- `pipeline_worker::do_cut()`/`do_validate()` reject such a document (before any cut/validate work) whenever `agent_id() == 0`. **The default (an unoverridden hook) returns `0`, not `std::nullopt`** -- a hook that never overrides this therefore has every document rejected, as a fail-closed default (a hook that genuinely wants every document processed unconditionally must override this and return a non-zero id explicitly, e.g. `return 1;`). `std::nullopt` is still a legitimate return value for a hook that overrides this and, for a specific `path`, cannot decide the agent at all -- distinct from "decided: unresolved" (`0`) -- and `agent_id() == 0` does **not** fire for an unset (`nullopt`) `agent_id()`.
- **`e_void on_wrk_start(int worker_id, cstr_t thread_name)`** / **`e_void on_wrk_end(int worker_id, cstr_t thread_name)`**
  - when: once per worker thread, when it starts / when it finishes
  - usage: per-thread setup/cleanup, e.g. open/close a database connection
  - parameters:
    - `worker_id` : `int` -- a small integer identifying this worker thread (0, 1, 2, ...)
    - `thread_name` : `cstr_t` -- this thread's own name, as it appears in log lines
- **`e_void on_doc_open(std::size_t doc_ndx, const doc_dscr& dscr)`**
  - when: 1. the document has been handed to a cutter thread; cutting is about to start
  - usage: log which document is being processed
  - parameters:
    - `doc_ndx` : `std::size_t` -- this document's index into the `xml_paths` vector you gave `exec()`
    - `dscr` : `const doc_dscr&` -- this one document's own description (e.g. `dscr.path()` for its file path)
- **`e_void on_doc_cutting_end(std::size_t doc_ndx, const doc_dscr& dscr)`**
  - when: 2. the document has been fully split into segments -- NOT a verdict, only "cutting is done" (renamed from the old, misleadingly-named `on_doc_close()`; see `on_doc_close()` below for the hook that fires once the document is actually closed)
  - usage: log that cutting is done
  - parameters:
    - `doc_ndx` / `dscr` -- same as `on_doc_open()`'s
- **`bool on_type(const YourSchemaClass& s, std::string_view raw_msg, const doc_dscr& dscr, segment_result& result, bool is_first, bool is_last)`**
  - when: 3. once per segment (dispatched from `on_seg_sem_check()`), including the document's header segment, in whatever order/thread each segment happens to be processed -- one overload per schema class in your namespace (see [Getting your data out](#13-getting-your-data-out) above)
  - usage: your own semantic validation
  - parameters:
    - `s` : `const YourSchemaClass&` -- the segment's own fields, fully typed (e.g. `s.msg_id`, `s.amount_sum` for a `pacs8_hdr`) -- exactly the members you annotated on that schema class, no string-based lookups needed
    - `raw_msg` : `std::string_view` -- the segment's raw XML fragment (`segment.view(dscr.mmf().data())`, computed once for you), for the rare case you need more than `s`/`result` already give you
    - `dscr` : `const doc_dscr&` -- this segment's own document's `doc_dscr` (`out_doc_id()`, `agent_id()`, `mmf()`, ...)
    - `result` : `segment_result&` -- the same segment, as a generic, name-indexed `segment_result` -- only useful if you need something `s` doesn't already give you fully typed (e.g. `result.seg_id()`); non-const because extracting a `validated_t<X>` field can append to `result.errors()` if that field's own validation fails
    - `is_first` / `is_last` : `bool` -- whether this is the first/last segment of its document (both `true` at once for a document with exactly one segment)
  - returns: this segment's semantic verdict -- `true` if it's fine, `false` if something about its content is wrong by your own business rules
- **`bool on_doc_sem_check(std::size_t doc_ndx)`**
  - when: 4. once, right after every one of the document's segments has gone through `on_type()` (independent of whether syntax/validation are known yet)
  - usage: cross-segment/document-wide checks (e.g. declared count in the header vs. actual number of transaction segments), via `doc_data(doc_ndx)`
  - parameters:
    - `doc_ndx` : `std::size_t` -- as above
  - returns: this document's own semantic verdict (`true` = ok), fed into the document's overall `doc_status_t` alongside syntax/validation
- **`e_void on_doc_stored(std::size_t doc_ndx, const doc_dscr& dscr)`**
  - when: 5. once, as soon as every one of the document's segments has actually been written out via `on_block_store()`/`on_failed_block_store()` (independent of steps 3/4 and of syntax/validation) -- fsp-core tracks this per document, not per flush batch, since one batch can mix segments from several documents and one document's segments can be flushed across several batches/worker threads
  - usage: your own "this document will never be written to again" signal -- e.g. mark it durably complete in your own storage, independent of the pass/fail verdict `on_doc_close()` below reports
  - parameters:
    - `doc_ndx` / `dscr` -- same as `on_doc_open()`'s
- **`bool on_doc_close(std::size_t doc_ndx, const doc_status_t& verdict, const error_info& err, const doc_dscr& dscr, std::size_t segments_stored)`**
  - when: 6. once, as soon as syntax + validation + step 4's semantic verdict + step 5's storage-completeness are ALL known -- whichever of those four facts happens to complete last is what triggers it; step 5 is now, by construction, always known before this fires
  - usage: log/act on the document's final verdict, e.g. move it to a done-path or an err-path
  - parameters:
    - `doc_ndx` / `dscr` -- as above
    - `verdict` : `const doc_status_t&` -- this document's final status: `verdict.ok()` for the overall pass/fail, `verdict.syntax_status()`/`valid_status()`/`semantic_status()` for the individual partial results (each a `fsp::three_state`), `verdict.is_finished()` guaranteed `true` here
    - `err` : `const error_info&` -- the syntax/validation error that failed the document, if any (default-constructed, i.e. "no error", otherwise)
    - `segments_stored` : `std::size_t` -- how many of this document's own segments actually left `on_block_store()`/`on_failed_block_store()` (`doc_counters::stored_count()`) -- guaranteed to already equal this document's own total segment count by this point (storage-completeness is what gates this hook's own timing), so this is really "how many segments this document had in total, confirmed durably written". `0` for a document rejected before any of its segments were ever processed (e.g. `get_doc_agent_id()` resolving to `0`) -- lets a caller skip storage-cleanup work outright when there is provably nothing to clean up
  - returns: the FINAL verdict you want recorded for this document (default: `verdict.ok()`)
- **`e_void on_doc_finish(std::size_t doc_ndx)`**
  - when: 7. once, immediately after `on_doc_close()` returns
  - usage: last read of `doc_data(doc_ndx)` before it's recycled for a different document
  - parameters:
    - `doc_ndx` : `std::size_t` -- as above; `doc_data(doc_ndx)` is still valid here, its timing already stopped, so `duration()` is safe to read
- **`e_void on_block_store(std::span<const std::size_t> indices, segment_pool& pool, const doc_set_dscr& ds_dscr)`** / **`e_void on_failed_block_store(std::span<const std::size_t> indices, std::span<const error_info> errors, segment_pool& pool, const doc_set_dscr& ds_dscr)`**
  - when: periodically, for a batch of successfully-validated/failed segments -- either once `ok_block_flush_size`/`nak_block_flush_size` worth have piled up, or once, with the remainder, at the end of a worker thread's run
  - usage: write results/failures out to your own storage (a file, a database, a message queue)
  - parameters:
    - `indices` : `std::span<const std::size_t>` -- pool slot indices belonging to this batch; not reused by anyone else until your call returns; can be empty (a harmless no-op call) for the final end-of-thread flush
    - `errors` : `std::span<const error_info>` (`on_failed_block_store()` only) -- one entry per `indices` (same order, same length): why `indices[i]` failed
    - `pool` : `segment_pool&` -- look up via `pool.segment_at(idx)`/`pool.result_at(idx)` to read the actual segment/result data
    - `ds_dscr` : `const doc_set_dscr&` -- the full document set; look up `ds_dscr[pool.segment_at(idx).doc_ndx()]` per index (a batch can mix segments from different documents) to resolve that document's own data, e.g. the id you returned from `get_doc_id()`

`on_type()` is the one you'll override most often -- declare one overload per schema class in
your namespace, with this exact parameter list:

```cpp
bool on_type(const fsp::work::pacs8_hdr& hdr, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override;
bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override;
```

```cpp
bool on_type(const fsp::work::pacs8_hdr& hdr, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override
{
  // hdr.msg_id, hdr.amount_sum, ... are already fully typed -- no string-based lookups needed.
  return true; // or false, if this segment fails your own business rule
}

bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override
{
  return txn.amount.value > 0; // your own business rule
}
```

The return value is that segment's semantic verdict: `true` means "this segment is fine", `false`
means "something about its content is wrong, even though it parsed correctly". This is different
from a segment that fails to parse at all (a syntax error) -- that kind of failure never reaches
`on_type()` in the first place.

A `static_assert` enforces that you declare an `on_type()` overload for *every* schema class in
the namespace, not just the ones you remember -- a missing or misspelled overload is a
compile-time error rather than a silently-skipped segment type. If a schema class genuinely has no
business rule of its own, its `on_type()` overload still needs to exist, just returning `true`
unconditionally, so that decision is visible in your own source instead of inferred from an
absence.

### 2.5. Run-level and document-level shared data

Besides your own plain member variables (safe per-thread, see the threading note above),
`typed_semantic_check`/`pipeline_hooks_crtp` give you two more places to keep state, each with a
lifetime the importer manages for you instead of you wiring it up by hand:

- **`run_data()`** -- one instance for the whole run, constructed right before `on_run_start()`
  and torn down right after `on_run_end()` returns. Unlike your own member variables, this is the
  **same object** on every worker clone -- exactly what you need for state that must be visible
  and updatable from any thread while the run is still going (e.g. a running total you don't want
  to wait until `on_run_end()`'s `worker_clones` sweep to compute).
- **`doc_data(doc_ndx)`** -- one instance per document, constructed before that document starts
  being cut and released back to an internal pool once you've had your last look at it (right
  after `on_doc_finish()` returns, see below). Also the same object across every thread that
  touches that document. Unlike a run-level instance, doc-level instances are **recycled**: once
  one document is done with its instance, the next document to start reuses the very same object
  (after `reset()` clears it) instead of the importer allocating a fresh one -- this matters when
  many documents are processed back to back, since it avoids an allocation/deallocation pair per
  document.

Both start out as the plain `fsp::run_data_root`/`fsp::doc_data_root` base classes, which give you
nothing but timing (see below). To add your own fields, derive your own struct from one of these
and name it as a template argument:

```cpp
struct my_run_data : fsp::run_data_root
{
  std::atomic<std::size_t> total_amount{0};
};

struct my_doc_data : fsp::doc_data_root
{
  std::size_t declared_count = 0;
  std::size_t actual_count   = 0;
  // Called by the importer right before a recycled instance is handed to the NEXT document --
  // clear your own fields here, then chain to the base so its timing is reset too.
  void reset() override
  {
    declared_count = 0;
    actual_count   = 0;
    fsp::doc_data_root::reset();
  }
};

class my_hooks : public fsp::typed_semantic_check<my_hooks, ^^fsp::work, fsp::seg_schema, my_run_data, my_doc_data>
{
  // run_data() now returns my_run_data&, doc_data(doc_ndx) returns my_doc_data&.
};
```

If a document type needs nothing beyond what an intermediate, package-level hook class already
added, it simply doesn't repeat the `RunData`/`DocData` template arguments -- ordinary C++ default
template arguments mean it automatically gets what the level above it already defined.

**Timing:** both base classes track when they started/stopped via a small `timing()` accessor:

```cpp
double secs = doc_data(doc_ndx).timing().duration().count(); // seconds since start(), or since
                                                              // end() if the timer has been stopped
```

`run_data()`'s timer stops right before `on_run_end()` runs; `doc_data(doc_ndx)`'s stops right
before `on_doc_finish()` runs (see the method table above) -- so `duration()` is always already
frozen and safe to read from inside either of those two hooks.

**Locking:** neither `run_data()` nor `doc_data(doc_ndx)` locks anything for you -- reading/
writing through them is exactly as fast as touching a plain reference. Most of the time that's
fine: your own plain member variables are already thread-confined (see the threading note above),
and a lot of doc-level state is only ever touched by whichever single thread happens to be
processing that document at a given point. Wrap the access in `fsp::lock()` only when you know
multiple threads genuinely can touch the SAME instance at the same time -- e.g. several worker
threads aggregating into the same `run_data()` from `on_type()`, or two independent hooks
(`on_doc_sem_check()` and the segment-processing path) racing to read/update the same `doc_data()`:

```cpp
// Example 1: run-level -- every worker thread's on_type() adds to the same running total.
bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override
{
  {
    auto guard = fsp::lock(run_data());
    guard->total_amount += txn.amount.value;
  } // unlocked here, at the end of the block
  return true;
}

// Example 2: doc-level -- on_type() accumulates the actual transaction count, on_doc_sem_check()
// compares it against the header's declared count -- both could, in principle, be reached from
// different threads for the same document.
bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
             fsp::segment_result& result, bool is_first, bool is_last) override
{
  auto guard = fsp::lock(doc_data(result.doc_ndx()));
  guard->actual_count += 1;
  return true;
}

bool on_doc_sem_check(std::size_t doc_ndx) override
{
  auto guard = fsp::lock(doc_data(doc_ndx));
  return guard->actual_count == guard->declared_count;
}
```

`fsp::lock(x)` returns an RAII guard (`fsp::locked_root<T>`): it locks `x`'s own mutex when
constructed and unlocks it automatically when the guard goes out of scope, so there's no
`lock()`/`unlock()` pair to remember or get wrong. Access the locked data through the guard with
`->`/`*`, same as a smart pointer.

### 2.6. Callback example

Here is a complete, minimal callback class that counts documents and segments, and applies a
simple validation rule. The callback below reads `txn.amount`, so first, here is the `fsp::work`
namespace (see [Getting your data out](#13-getting-your-data-out) above) it dispatches segments
against -- this is what defines that `pacs8_txn` even has an `amount` field to read:

```cpp
// work.hpp
#pragma once
#include "reflection.hpp"

namespace fsp::work
{
  class [[= "/Document/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
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
#include "typed_semantic_check.hpp"
#include "work.hpp"

class my_hooks : public fsp::typed_semantic_check<my_hooks, ^^fsp::work>
{
public:
  std::size_t documents_seen = 0;
  std::size_t segments_ok    = 0;
  std::size_t segments_error = 0;

  bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
               fsp::segment_result& result, bool is_first, bool is_last)
  {
    const bool ok = txn.amount.value > 0; // your own business rule
    if (ok) ++segments_ok;
    else ++segments_error;
    return ok;
  }
protected:
  fsp::e_void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr) override
  {
    ++documents_seen;
    log().info(fmt::format("Started document {}: '{}'", doc_ndx, dscr.path()));
    return {};
  }

  fsp::e_void on_run_end(const fsp::doc_set_counter&           counters,
                         const fsp::doc_set_dscr&              ds_dscr,
                         std::span<const fsp::pipeline_hooks*> worker_clones) override
  {
    std::size_t total_ok = segments_ok, total_error = segments_error;
    for (const auto* clone : worker_clones)
    {
      const auto* c = static_cast<const my_hooks*>(clone); // safe: every clone IS a my_hooks
      total_ok    += c->segments_ok;
      total_error += c->segments_error;
    }
    log().info(fmt::format("Run finished: {} ok, {} failed", total_ok, total_error));
    return {};
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

## 3. Simple example

This is the smallest complete program that imports a set of XML files and prints a summary --
no callback needed. See `src/test/pacs8.cpp` for the full, buildable version this is based on.

```cpp
#include "importer.hpp"
#include "exe_path.hpp"
#include "work.hpp" // your own target classes, see "Getting your data out" above

int main(int argc, const char* argv[])
{
  std::vector<fsp::str_t> xml_paths = { /* ... your files ... */ };
  fsp::cstr_t              xsd_path = "schema.xsd"; // or "" to skip schema validation

  // log.conf is copied next to the binary at build time (see add_log_config() in
  // CMakeLists.txt, which already picked the right one of config/log.debug.json /
  // config/log.release.json for this build's own CMAKE_BUILD_TYPE) -- resolved against
  // fsp::exe_dir() (the running binary's own directory), not the current working directory.
  auto log_cfg      = logger::load_logger_config((fsp::exe_dir() / "log.conf").string());
  log_cfg.app_name  = "my_importer";

  auto cfg = fsp::importer_config{
    .targets        = fsp::proc_data_of<^^fsp::work>(), // built from work.hpp's fsp::work namespace -- see "Getting your data out" above
    .num_of_workers = 16,
    .log_config     = log_cfg,
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

`cfg.targets` is the one field that actually ties this program to `work.hpp`: `fsp::proc_data_of<^^fsp::work>()`
walks the `fsp::work` namespace at compile time (via C++26 reflection) and builds the full
`proc_data` the importer needs to know how to cut/extract -- every other `importer_config` field
is generic run configuration, unrelated to which XML elements you're after (see
[Getting your data out](#13-getting-your-data-out) above for what goes into that namespace, and
[`importer_config`](#12-importer_config) above for the rest of these fields).

That's it: describe your target elements once (`work.hpp`), call `exec()`, check the result. Add
a callback class only once you need to react to individual documents/segments while the run is
still in progress.

## 4. Complex example

This example puts everything from this document together into one working program, based on
this repository's own `pacs8`/`pacs8-cb` demo (`src/test/work.hpp`, `pacs8_cb.hpp`/`.cpp`,
`pacs8-cb.cpp`), trimmed down to a single schema class for readability. It has all four pieces:

1. the namespace that defines how the document is cut into segments,
2. the class members that define what `on_type()` gets,
3. the callback itself, and
4. the main program tying it all together, `#include`s included.

### 4.1. The namespace: how the document is cut

```cpp
// work.hpp
#pragma once
#include "reflection.hpp"

namespace fsp::work
{
  // Every element matching this XPath becomes one segment, processed as a pacs8_txn.
  class [[= "/Document/x:FIToFICstmrCdtTrf/x:CdtTrfTxInf"]] pacs8_txn : public fsp::seg_schema
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
extraction, before `on_type()` even runs, and reported through `result.errors()` on failure (see
`result`'s own parameter description in
[Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics) above). Plain
`amount_t` here keeps this example focused on the callback itself.)

### 4.2. The class members: what `on_type()` gets

The four annotated members above (`txn_id`, `amount`, `currency`, `debtor_bic`) are exactly what
ends up readable, fully typed, in `on_type()`'s own `txn` parameter -- see step 3 below, where
`txn.txn_id`/`txn.amount` are used directly, with no string-based lookups.

### 4.3. The callback

```cpp
// my_pacs8_hooks.hpp
#pragma once
#include "typed_semantic_check.hpp"
#include "work.hpp"

class my_pacs8_hooks : public fsp::typed_semantic_check<my_pacs8_hooks, ^^fsp::work>
{
public:
  std::size_t segments_ok    = 0;
  std::size_t segments_error = 0;

  bool on_type(const fsp::work::pacs8_txn& txn, std::string_view raw_msg, const fsp::doc_dscr& dscr,
               fsp::segment_result& result, bool is_first, bool is_last)
  {
    // Our own business rule: a transaction must carry a positive amount. amount_t stores its
    // value scaled by 10^amount_scale (see parsing_util.hpp), as a plain .value member.
    const bool valid = txn.amount.value > 0;
    if (! valid) log().warn(fmt::format("txn {} failed validation: amount={}", txn.txn_id, txn.amount));
    if (valid) ++segments_ok;
    else ++segments_error;
    return valid;
  }
protected:
  fsp::e_void on_doc_open(std::size_t doc_ndx, const fsp::doc_dscr& dscr) override
  {
    log().info(fmt::format("Started document {}: '{}'", doc_ndx, dscr.path()));
    return {};
  }

  fsp::e_void on_run_end(const fsp::doc_set_counter&           counters,
                         const fsp::doc_set_dscr&              ds_dscr,
                         std::span<const fsp::pipeline_hooks*> worker_clones) override
  {
    std::size_t total_ok = segments_ok, total_error = segments_error;
    for (const auto* clone : worker_clones)
    {
      const auto* c = static_cast<const my_pacs8_hooks*>(clone); // safe: every clone IS a my_pacs8_hooks
      total_ok    += c->segments_ok;
      total_error += c->segments_error;
    }
    log().info(fmt::format("Run finished: {} ok, {} failed", total_ok, total_error));
    return {};
  }
};
```

### 4.4. The main program

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
      .log_config     = load_program_logger_config(args.bare_name),
      .program_name   = args.bare_name,
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

## 5. A note on the class hierarchy

Everything above is written against `fsp::typed_semantic_check<YourClass, ^^YourNamespace>` --
the base every example in this document actually derives from, and the one you should use too.
It sits between two lower-level pieces you won't normally touch directly, but that are worth
knowing about if you ever need to go past `on_type()`:

- **`fsp::pipeline_hooks`** -- the actual base class every hook object is, underneath. Its
  own extension point for "check this segment" is `on_seg_sem_check(const xml_segment& segment,
const doc_dscr& dscr, segment_result& result, bool is_first, bool is_last)`: one single virtual
  call for *any* segment type, given to you as the generic, name-indexed `segment_result` -- no
  per-schema-class dispatch at all. `typed_semantic_check` implements `on_seg_sem_check()` once
  and turns it into the `on_type()` overloads you actually write.
- **`fsp::materialize_variant<^^YourNamespace>(result.seg_type(), result)`** -- the call
  `typed_semantic_check`'s own `on_seg_sem_check()` makes internally to turn that generic
  `segment_result` into a `std::variant` of your schema classes, then `std::visit()`s it straight
  into the matching `on_type()` overload. `result.seg_type()` is the integer index (in
  declaration order) of which schema class this segment is -- already recorded when the segment
  was cut, not something you set yourself.

You would only override `on_seg_sem_check()` directly (deriving from
`fsp::pipeline_hooks_crtp<YourClass>` instead of `typed_semantic_check`) if per-schema-class
dispatch genuinely doesn't fit what you need -- e.g. a single validation rule that reads generic,
name-indexed values without caring which schema class they came from. For the overwhelming
majority of callers, `on_type()` is simpler, is compile-time-checked for missing schema classes
(see [Method purpose and parameter semantics](#21-method-purpose-and-parameter-semantics) above), and
is what every example in this document uses.

## 6. Document errors

> **Parts of this section are implemented; the rest remains a design proposal.** See
> [6.0](#60-what-is-implemented-vs-what-remains-a-proposal) immediately below for exactly which is
> which. `doc_status_t` (see [7.2.3](#723-doc_status_t)) still carries its original four
> `three_state` facts (syntax/valid/semantic/stored) with one aggregate `ok()`/`status()`
> verdict, UNCHANGED -- the UA/SE/VE/HE/TE taxonomy below is implemented as an ADDITIONAL,
> orthogonal `error_class` bitmask alongside those four facts, not a replacement for them, and not
> the five-way bitmask-as-primary-state design 6.1 originally sketched. The per-document
> instruction queues (6.3/6.4) remain an unimplemented proposal. HE now rejects the whole document
> exactly like UA/SE/VE (`doc_dscr::rejected()` turns `true` the moment `pipeline::
> report_error_class()` records it, whichever error_class it is) - see 6.0's own "Implemented" list
> for why a header semantic failure is no longer a narrower case than a syntax/validation one.

### 6.0. What is implemented vs. what remains a proposal

**Implemented:**

- **`fsp::error_class`** (`doc_dscr.hpp`) -- an `enum class : std::uint8_t` with one bit per class
  (`ua`/`se`/`ve`/`he`/`te`). `doc_status_t::mark_error(error_class)` OR's a bit in; `error_mask()`/
  `has_error(error_class)` read it back. Purely additive: `status()`/`ok()`/`syntax_status()`/etc.
  are computed exactly as before this existed, from the original four `three_state` facts only --
  `error_mask_` is a second, independent record of WHICH class(es) of error a document was marked
  with, not a new source of truth for whether it passed. A document can accumulate more than one
  bit over its lifetime (e.g. a failed transaction segment recorded as `te`, followed later by the
  header itself also failing as `he`).
- **UA/SE/VE marking** -- `pipeline_worker::do_cut()`/`do_validate()` call `pipeline::report_error_class()`
  (which wraps `mark_error()`) at each of their own three rejection sites: `agent_id()==0` (`ua`), a
  well-formedness failure from either C or V (`se`), a genuine XSD schema violation from V (`ve`).
  See [pipeline_worker.cpp](../src/importer/pipeline_worker.cpp).
- **HE/TE marking** -- `pipeline::check_segment_semantics()` looks up the FAILING segment's own
  `subtree_type()` against `cfg_.targets.is_header` (the same vector `hdr_seg_schema`-derived
  classes compile into, see [1.3.2](#132-marking-a-schema-class-as-a-header-segment)) the moment
  `on_type()` returns `false`, and marks `he` or `te` accordingly. This fires independently of
  `doc_status_t::semantic_` itself -- `semantic_` is set separately, once, from
  `on_doc_sem_check()`'s own document-wide verdict (see [2.3](#23-segment-processing-order)) -- so
  `has_error(error_class::he)` can already be `true` while `semantic_status()` is still `unknown`
  or even `valid`, if the cb's own `on_doc_sem_check()` doesn't itself factor the segment failure
  in. A lone `te` does not, by itself, reject the document (see [pipeline_hooks.hpp](../src/importer/pipeline_hooks.hpp)'s
  own doc comment on `on_remove_stored_data_safe()` for why).
- **`on_remove_stored_data(out_doc_id, no_headers)`** (see [2.4](#24-callback-interface)) -- a real
  hook, called via `pipeline::report_error_class()` the FIRST time any `error_class` bit is ever
  recorded for a document (`mark_error()`'s own return value distinguishes "first bit" from
  "another bit on an already-marked document"), so it fires **at most once per document**, for
  UA/SE/VE/HE (never for a lone TE). `no_headers` is `false` for UA/SE/VE (the whole document is
  unusable), `true` for HE (only the header record itself is not what needs removing). fsp does
  not track which of a cb's own writes belong to `out_doc_id` -- that indexing, and how the cb
  interprets `no_headers`, is entirely the cb's own responsibility, the same way `on_block_store()`'s
  own writes already are. A failed rollback (returned as an error) is logged and the run continues
  -- same "log and move on" handling as a failed `on_block_store()` call, not a fatal, run-stopping
  error (see [xml_worker.cpp](../src/importer/xml_worker.cpp)'s `flush_ok_block()`).
- **`doc_dscr::rejected()`'s lock-free fast path** -- `doc_status_t::rejected()` is now a plain
  `std::atomic<bool>` (relaxed load), instead of computing `status() == three_state::invalid` on
  every call. Set (also relaxed, one-way, never reset) from TWO independent places, whichever
  reaches it first: `set_field()`, the moment any of `syntax_`/`valid_`/`semantic_` first turns
  invalid, AND `pipeline::report_error_class()`'s own `mark_rejected()` call, unconditionally for
  every `error_class` (UA/SE/VE/HE alike - see `doc_dscr::mark_rejected()`'s own doc comment for
  why HE joins the other three: a header semantic failure is exactly as fatal to the rest of a
  document's own transactions as a header the XSD itself rejected would be, so it must reject the
  document just as early - `report_error_class()` runs BEFORE the corresponding `set_field()` call
  for HE's own `semantic_`, which only turns invalid later, once `on_doc_sem_check()`'s own
  document-wide verdict is known). `status()` itself, and every other reader of the four
  `three_state` facts, is unchanged -- only this one, per-segment hot-path question moved off
  `status()`'s own `std::scoped_lock`. See `xml_worker::process_one()`'s own `rejected()` call for
  where this matters at scale (checked once per segment, so a mutex there would be paid millions of
  times per run for a question that is "no" the overwhelming majority of the time) - and for why HE
  rejecting early matters in practice: a transaction segment of the SAME document, still in flight
  on another worker thread, is now skipped outright ("document invalid, segment skipped") instead
  of being processed and semantically checked despite the header it depends on already being known
  bad.
- **Tests** -- direct, pipeline-free coverage of `error_class`/`mark_error()`/`error_mask()`/
  `has_error()`/`rejected()` in `test_doc_status_t.cpp`; end-to-end pipeline coverage (UA, SE, VE,
  HE, TE, and a multi-class-on-one-document case, each asserting both the resulting `error_mask()`
  and whether/how `on_remove_stored_data_safe()` fired) in `test_pipeline_stages.cpp`.

**Still a proposal, not implemented:**

- The original **five-class bitmask-as-primary-state** design 6.1 first sketched (`|= UA` etc. as
  the document's OWN state, not an addition alongside `doc_status_t`'s four facts). What's
  implemented instead keeps `doc_status_t`'s four-fact model as the sole source of truth for
  pass/fail, with `error_class` purely as an additional annotation -- see "Implemented" above.
- Any **instruction queue** (the "(1)"/"(2)" queues in 6.3/6.4) that pushes a live signal to other
  threads the moment an error is detected. `xml_worker::process_one()`'s existing poll
  (`rejected()`, checked per segment) remains the only cleanup-triggering mechanism; C
  (`do_cut()`)/V (`do_validate()`) still only check `agent_id()`/`failed()` against the ONE
  document they are about to start on, not a per-role "clean up before next assignment" checkpoint
  -- see 6.0's earlier gap-analysis discussion for the full reasoning on why a push queue was not
  judged worth its added complexity over the existing poll.
- The **HE-specific "non-header segments only" narrowing** of in-flight skipping (6.6). Today,
  `xml_worker::process_one()`'s `rejected()` check is document-wide: once ANY error class rejects a
  document (UA/SE/VE/HE all included, since `report_error_class()` calls `mark_rejected()`
  unconditionally), every remaining segment of that document is skipped uniformly -- there is no
  code path that still processes a document's remaining HEADER segment(s) while skipping only its
  non-header ones (moot in practice: a document only ever has one header segment, and it is always
  either the one that itself failed as HE, or already processed before HE could even be recorded).
  (`on_remove_stored_data()`'s own `no_headers=true` for HE only tells a cb what to remove from
  storage it ALREADY wrote before the rejection was known -- it does not change which in-flight
  segments C/V/P still process afterward.)
- **Per-error-class differentiated skip rules for V/C/P** (6.6's `{UA, SE, VE(?), HE}` vs.
  `{UA, SE, VE}` sets) beyond the single, uniform `rejected()` boolean - `rejected()` today does
  not distinguish which error_class caused it, so V/C/P's own skip decision (via `rejected()`
  alone) cannot be tuned per class the way 6.6 sketches (e.g. a hypothetical "V keeps validating a
  document already known HE, C does not" policy).

FSP is intended for fast SEPA/XML processing, but errors are part of the real-world workload it has
to handle. The system is capable of detecting several classes of error; how that's reflected for a
specific use case is up to the application programmer.

### 6.1. Error classes

1. **UA** -- unknown agent: the document arrived through an unknown agent that cannot be trusted.
   Stop processing as soon as possible.
2. **SE** -- syntax error: the document is not XML-compliant. Nothing about it can be assumed, so
   it should be dropped immediately rather than spend further resources on it.
3. **VE** -- validation error: the document failed XSD validation. Same category of error as a
   syntax error, with the same consequences.
4. **HE** -- header semantic error: the header is what shapes the whole document.
5. **TE** -- transaction semantic error.

### 6.2. Error detection

1. **UA** -- detected by `pipeline_hooks::get_doc_agent_id(cstr_t path)`, so what counts as
   "unknown agent" is an application-programmer decision. Detected on the main thread.
2. **SE** -- detected by the cutter (C thread/worker) -- a SAX error.
3. **VE** -- detected by the validator (V thread/worker) or by the cutter (C thread/worker) -- an
   XSD error.
4. **HE** -- detected by the segment semantic check (`on_type()`) -- checked by the pipeline after
   `on_type()` finishes and its `true`/`false` result is available. A `false` result on the header
   segment makes it HE.
5. **TE** -- detected by the segment semantic check (`on_type()`) -- checked by the pipeline after
   `on_type()` finishes and its `true`/`false` result is available. A `false` result on a
   non-header segment makes it TE.

### 6.3. Error notification

1. **UA** -- detected by the main thread.
   1. No need to notify other threads.
   2. Document state: `|= UA`.
2. **SE** -- detected by the cutter.
   1. Signal other threads (instruction queue (1)) to drop processing of the document with a
      syntax error.
   2. Document state: `|= SE`.
3. **VE** -- detected by the cutter or the validator.
   1. Signal other threads (instruction queue (1)) to drop processing of the document with a
      validation error.
   2. Document state: `|= VE`.
4. **HE** -- detected by the pipeline, after the header segment type comes back with a `false`
   result.
   1. Signal other threads (instruction queue (2)) to drop processing of the document's
      non-header segments.
   2. Document state: `|= HE`.
5. **TE** -- detected by the pipeline, after a non-header segment type comes back with a `false`
   result.
   1. No need to notify other threads.
   2. Document state: `|= TE`.

### 6.4. Error cleanup -- cached data

A thread needs to clean up its own cached data that hasn't yet been written to permanent storage.
Each thread receives instructions via its own instruction queue:

- `1` -- clear the document's data (all segments).
- `2` -- clear the document's non-header data (only non-header segments).

### 6.5. Error cleanup -- permanent storage

Call the `on_remove_stored_data(doc_id, type)` callback (`type` being `all` or `non-header`). This
hook is called by the pipeline/thread that detected the error.

### 6.6. Error cleanup -- ongoing

> As implemented, this is simpler than the differentiated per-class skip rules originally sketched
> below: `xml_worker::process_one()`'s `rejected()` check is a single, uniform boolean (see 6.0's
> own "Implemented" list) - P skips ALL remaining segments of a document rejected on ANY class,
> UA/SE/VE/HE alike, never just its non-header ones. The distinction this subsection originally
> drew between "skip non-header only" (HE) and "skip everything" (UA/SE/VE) is therefore not
> something the current code makes.

After the initial cleanup, some segments belonging to erroneous documents may still be in flight --
threads need to check each segment before processing it and skip it if necessary:

- **Validator** -- skips documents with `{UA, SE, VE(?), HE}`.
- **Cutter** -- skips documents with `{UA, SE, VE(?), HE}`.
- **Processor (P thread)**:
  - skips non-header segments belonging to documents with `{UA, SE, VE, HE}`;
  - skips all segments belonging to documents with `{UA, SE, VE}`.

## 7. Internals

Everything above is what a caller needs. This section documents internal
data structures. Useful background if you're reading fsp-core's own source, or if a hook
signature hands you one of these and you want to know exactly what else it offers beyond the
handful of accessors this document already covers inline (`dscr.path()`, `verdict.ok()`, and so
on).

### 7.1. Waiting queues

The pipeline coordinates its C(cutter)/V(validator)/P(processor) worker threads through a set of
`lock_queue<T>` instances (`lock_queue.hpp`) -- a mutex + condition-variable queue with blocking
`pop()`, non-blocking `try_pop()`, and a `finished`/`aborted` shutdown state every queue in this
section shares (see `lock_queue.hpp`'s own doc comments for the full API). This subsection lists
every queue actually instantiated in the pipeline today.

#### 7.1.1. The instruction queue (design proposal -- not yet implemented)

> As with the still-unimplemented parts of [6. Document errors](#6-document-errors) above (see its
> own [6.0](#60-what-is-implemented-vs-what-remains-a-proposal)), this subsection describes a
> possible future mechanism, not something that exists in the code. None of the queues in
> [7.1.2](#712-queues-that-exist-today) below carry instruction-queue-shaped entries
> (`sender`/`i_type`/`data`) -- this is kept here only because it's the cross-thread notification
> mechanism section 6's own still-unimplemented parts would depend on.

The instruction queue is a way to propagate instructions from one thread to every other thread. It
would be made of N queues, where N is the number of worker threads the importer runs. If thread `i`
wants to instruct the other threads, it would fill the instruction into every instruction queue
except its own (the `i`-th one).

Proposed queue entry shape:

- `sender` (`uint16_t`) -- id of the sending thread.
- `i_type` (`uint8_t`) -- instruction type/code: `1` = clear document data, `2` = clear document
  non-header data.
- `data` (`uint32_t`) -- data associated with the instruction; its meaning depends on `i_type`.

#### 7.1.2. Queues that exist today

| Attribute                       | Type                                                  | Semantics                                                                                                                                                                                                                                              |
| -------------------------------- | ------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `pipeline::c_queue_`             | `lock_queue<std::size_t>`                               | Document indices waiting to be cut (C). One instance, shared by every worker thread; `try_pop_cut()`/`pipeline_worker::do_cut()` drain it, `pipeline::seed_queues()` fills it once at run start. |
| `pipeline::v_queue_`             | `lock_queue<std::size_t>`                               | Document indices waiting to be validated (V), only used in separate-pass mode (see [`cut_with_validation`](#121-cut_with_validation)). One instance, shared by every worker thread; `try_pop_validate()`/`pipeline_worker::do_validate()` drain it. |
| `segment_pool::ready_queues_`    | `std::vector<lock_queue<std::size_t>>` (one per shard) | Ordinary (non-header) segment slot indices ready for a P-role thread to process, produced by the cutter's `Handler::endElement()` via `push_ready()`. Sharded by `importer_config::pool_shard_count` to cut lock contention between concurrent C/P threads (see [`pool_shard_count`](#124-pool_shard_count)); a thread tries its own shard first, then sweeps the others. |
| `segment_pool::header_ready_queues_` | `std::vector<lock_queue<std::size_t>>` (one per shard) | Same role as `ready_queues_`, but only for segments whose schema class derives from `hdr_seg_schema` (see [1.3.2](#132-marking-a-schema-class-as-a-header-segment)). Every P-role thread drains this set first, before ever looking at `ready_queues_`, so a header segment can never be starved behind an unbounded pile of ordinary ones (see [2.3.1](#231-header-segments-are-processed-first)). |
| `segment_pool::free_queues_`     | `std::vector<lock_queue<std::size_t>>` (one per shard) | Pool slot indices no longer in use, ready to be handed back out by `acquire_slot()`. A P-role thread pushes a slot back here (`release_slots()`) once it's fully done with a segment (past any storage hook); the cutter pops from here before ever growing the pool's own high-water mark. |

`segment_pool.hpp` also declares a `using ndx_queue = lock_queue<std::size_t>;` alias and
`xml_worker.hpp` a `using segment_queue = lock_queue<xml_segment>;` one -- neither backs an actual
member of any class; they're unused type aliases, not additional queues.

### 7.2. Document related structures

#### 7.2.1. `doc_set_dscr`

The whole-run collection of documents: one `doc_dscr` per XML file passed to `exec()`, plus the
(optional) XSD grammar document. `pipeline_hooks::on_run_start()`/`on_run_end()` are handed a
`const doc_set_dscr&`; indexing it (`ds_dscr[doc_ndx]`) is how a hook gets from a `doc_ndx` to
that document's own `doc_dscr` outside the hooks that already receive one directly (e.g.
`on_block_store()`, see [Batch storage hooks](#233-batch-storage-hooks) above).

Non-copyable (holds a `std::vector<doc_dscr>`, and `doc_dscr` itself is non-copyable), move-only
in a restricted sense (`operator=(doc_set_dscr&&)` is deleted -- only construction can move).

**Attributes:**

| Member     | Type                    | Meaning                                                                                             |
| ---------- | ----------------------- | --------------------------------------------------------------------------------------------------- |
| `log_`     | `const logger::Logger&` | this run's logger; must outlive the `doc_set_dscr` -- stored as a reference, never copied           |
| `doc_set_` | `std::vector<doc_dscr>` | one entry per XML file, in the same order as the `xml_paths` vector given to `exec()`               |
| `grammar_` | `doc_dscr`              | the XSD schema file, as its own `doc_dscr`; default-constructed (not open) when no schema was given |

**Methods:**

| Signature                                                                          | Semantics                                                                                                                              |
| ---------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `explicit doc_set_dscr(const logger::Logger& logger, size_type initial_size = 16)` | constructs an empty set, reserving `initial_size` slots up front (`doc_set_.reserve()`)                                                |
| `bool add_document(cstr_t path)`                                                   | opens `path` as a new `doc_dscr`, appends it; `false` on an empty path or if opening throws (caught and logged, not propagated)        |
| `bool add_document(doc_dscr&& doc)`                                                | appends an already-constructed `doc_dscr` by moving it in; `false` only if the move itself throws                                      |
| `bool set_grammar(cstr_t path)`                                                    | opens `path` as the grammar document; `false` on an empty path or a caught exception                                                   |
| `bool set_grammar(doc_dscr&& doc)`                                                 | sets the grammar document by moving an already-constructed `doc_dscr` in                                                               |
| `[[nodiscard]] doc_dscr& operator[](size_type pos)` / `const` overload             | indexed access, no bounds check beyond what the const overload also throws on                                                          |
| `[[nodiscard]] doc_dscr& at(size_type pos)` / `const` overload                     | same as `operator[]`, but explicitly bounds-checked (throws `std::out_of_range`, and the non-const overload also logs the error first) |
| `[[nodiscard]] doc_dscr& grammar() noexcept` / `const` overload                    | the grammar document's own `doc_dscr` -- check `has_grammar()` first; a default-constructed (unopened) `doc_dscr` otherwise            |
| `[[nodiscard]] size_type size() const noexcept`                                    | number of documents in the set (not counting the grammar document)                                                                     |
| `[[nodiscard]] bool empty() const noexcept`                                        | `size() == 0`                                                                                                                          |
| `void clear() noexcept`                                                            | empties `doc_set_` (grammar document untouched); logs how many were dropped                                                            |
| `void reserve(size_type new_capacity)`                                             | forwards to `doc_set_.reserve()`                                                                                                       |
| `[[nodiscard]] bool has_grammar() const noexcept`                                  | `true` iff the grammar document is open (`static_cast<bool>(grammar_)`)                                                                |
| `begin()`/`end()`/`cbegin()`/`cend()` (const and non-const)                        | iterate `doc_set_` directly -- a range-`for` over a `doc_set_dscr` visits every document, not the grammar                              |
| `[[nodiscard]] const logger::Logger& log() const noexcept`                         | this run's logger                                                                                                                      |
| `[[nodiscard]] const std::vector<doc_dscr>& doc_set() const` / non-const overload  | direct access to the underlying vector, for callers that need vector-specific operations the wrapper above doesn't expose              |
| `[[nodiscard]] cstr_t xsd_file() const`                                            | the grammar document's own path, or `""` if `has_grammar()` is false                                                                   |

#### 7.2.2. `doc_dscr`

One document's own descriptor: the memory-mapped file itself (`mmap_file`), its syntax/
validation/semantic/stored verdict (a `doc_status_t`, see [7.2.3](#723-doc_status_t) below), the
first error recorded against it (if any), and the two caller-assigned opaque ids
(`out_doc_id()`/`agent_id()`) a hook can attach to it. This is the type every `dscr`/`doc_dscr&`
parameter throughout [2. callback description](#2-callback-description) above refers to.

Non-copyable, move-only. `close()`/destruction close the underlying `mmap_file` if still open.

**Attributes:**

| Member           | Type                          | Meaning                                                                                                              |
| ---------------- | ----------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| `doc_`           | `mmap_file`                   | the memory-mapped document itself -- byte access, `path()`, `string_view()`, etc. all forward to this                |
| `status_`        | `doc_status_t`                | syntax/validation/semantic/stored verdict and completion logic, see [7.2.3](#723-doc_status_t) below                    |
| `err_mutex_`     | `std::mutex`                  | guards `err_set_`/`err_` against the first-writer-wins race described under `note_error_once()` below                |
| `err_set_`       | `bool`                        | `true` once a failure reason has been recorded (`note_error_once()` has run at least once)                           |
| `err_`           | `error_info`                  | the recorded failure reason -- whichever of `set_syntax_result()`/`set_validation_result()` reported a failure first |
| `out_doc_id_`    | `std::uint64_t`               | caller-assigned output document id, see `get_doc_id()`; `0` until set                                                |
| `agent_id_`      | `std::optional<std::int16_t>` | caller-assigned agent id, see `get_doc_agent_id()`; set exactly once, in `pipeline::add_documents()`, to whatever that call returned -- `0` unless the hook overrides it (see `get_doc_agent_id()`'s own doc comment) |
| `open_reported_` | `mutable std::atomic<bool>`   | `true` once `on_doc_safe_open()` has returned for this document, see `mark_opened()`/`is_opened()` below             |

**Methods:**

| Signature                                                                                                                            | Semantics                                                                                                                                                                                                                                         |
| ------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `doc_dscr()`                                                                                                                         | default-constructed, unopened document                                                                                                                                                                                                            |
| `explicit doc_dscr(cstr_t path)`                                                                                                     | opens `path` as a memory-mapped file immediately                                                                                                                                                                                                  |
| `explicit doc_dscr(mmap_file&& file)`                                                                                                | wraps an already-open `mmap_file`                                                                                                                                                                                                                 |
| `void close() noexcept`                                                                                                              | closes the underlying `mmap_file`, if open                                                                                                                                                                                                        |
| `[[nodiscard]] bool is_open() const noexcept`                                                                                        | forwards to `mmap_file::is_open()`                                                                                                                                                                                                                |
| `[[nodiscard]] bool empty() const noexcept`                                                                                          | forwards to `mmap_file::empty()`                                                                                                                                                                                                                  |
| `[[nodiscard]] size_t size() const noexcept`                                                                                         | forwards to `mmap_file::size()`                                                                                                                                                                                                                   |
| `[[nodiscard]] const std::byte* data() const noexcept`                                                                               | forwards to `mmap_file::data()`                                                                                                                                                                                                                   |
| `[[nodiscard]] cstr_t path() const`                                                                                                  | this document's own file path, as given to `exec()`/`add_document()`                                                                                                                                                                              |
| `[[nodiscard]] cstr_t string_view() const`                                                                                           | the whole document's content, as a `cstr_t`                                                                                                                                                                                                       |
| `[[nodiscard]] std::byte operator[](size_t pos) const` / `at(size_t pos) const`                                                      | byte access, unchecked / bounds-checked (`at()` throws `std::out_of_range`)                                                                                                                                                                       |
| `begin()`/`end()`/`cbegin()`/`cend() const noexcept`                                                                                 | byte-level iteration over the mapped file                                                                                                                                                                                                         |
| `[[nodiscard]] std::span<const std::byte> span() const noexcept`                                                                     | the whole mapped file, as a span                                                                                                                                                                                                                  |
| `[[nodiscard]] std::span<const std::byte> subspan(size_t offset, size_t count) const`                                                | a sub-range of the mapped file                                                                                                                                                                                                                    |
| `void prefetch(size_t offset, size_t count = mmap_file::prefetch_size) const noexcept`                                               | hints the OS to page in `[offset, offset+count)` ahead of an upcoming read                                                                                                                                                                        |
| `[[nodiscard]] explicit operator bool() const noexcept`                                                                              | `is_open()`                                                                                                                                                                                                                                       |
| `[[nodiscard]] const mmap_file& mmf() const noexcept` / non-const overload                                                           | direct access to the underlying `mmap_file`, for advanced use beyond the forwarding accessors above                                                                                                                                               |
| `[[nodiscard]] doc_status_t& status() noexcept` / `const` overload                                                                   | the live `doc_status_t` itself (a reference, not a snapshot -- `doc_status_t` is non-copyable), see [7.2.3](#723-doc_status_t) below                                                                                                                 |
| `[[nodiscard]] bool failed() const noexcept`                                                                                         | `true` once syntax or validation is already known invalid (semantic_ deliberately excluded -- see its own doc comment); the C/P "already known invalid, skip this segment" precondition                                                           |
| `[[nodiscard]] bool rejected() const noexcept`                                                                                       | `true` once ANY of syntax/validation/semantic is known invalid (semantic_ included, unlike `failed()`); the flush-time "drop this segment, its document is already doomed" predicate                                                              |
| `[[nodiscard]] bool set_syntax_result(bool ok, bool folded_validation, error_info err = {}) noexcept`                                | reported by C once cutting finishes; `folded_validation` selects whether this call alone also sets validation (see its own doc comment for the folded-vs-separate-V rule); returns `true` iff this call must go on to call `hooks.on_doc_close()` |
| `[[nodiscard]] bool set_validation_result(bool ok, error_info err = {}) noexcept`                                                    | reported by V (a separate validation pass, only when not folded); same return-value meaning as `set_syntax_result()`                                                                                                                              |
| `[[nodiscard]] bool set_semantic_result(bool ok) noexcept`                                                                           | reported by the worker that wins the "all segments processed" completion check, right after `on_doc_sem_check()` returns; same return-value meaning                                                                                               |
| `[[nodiscard]] bool set_stored_result() noexcept`                                                                                    | reported by `pipeline::record_segments_stored()` once this document's stored-segment count reaches its known total; same return-value meaning; see `doc_status_t::set_stored()` for why it takes no `ok` parameter                                |
| `[[nodiscard]] const error_info& error() const noexcept`                                                                             | the first-recorded failure reason, or a default-constructed (empty) `error_info` if none was ever recorded                                                                                                                                        |
| `[[nodiscard]] std::uint64_t out_doc_id() const noexcept` / `void set_out_doc_id(std::uint64_t id) noexcept`                         | caller-assigned output document id, see `get_doc_id()` above                                                                                                                                                                                      |
| `[[nodiscard]] std::optional<std::int16_t> agent_id() const noexcept` / `void set_agent_id(std::optional<std::int16_t> id) noexcept` | caller-assigned agent id, see `get_doc_agent_id()` above                                                                                                                                                                                          |
| `void mark_opened() const noexcept`                                                                                                  | records that `on_doc_safe_open()` has returned for this document (called once, by `pipeline_worker::do_cut()`)                                                                                                                                    |
| `[[nodiscard]] bool is_opened() const noexcept`                                                                                      | `true` once `mark_opened()` has run; guards against V racing ahead of C and observing a document that hasn't been opened yet (see its own doc comment)                                                                                            |

#### 7.2.3. `doc_status_t`

The mutex-protected, four-fact completion gate behind `doc_dscr::status()` -- owns the
syntax/validation/semantic/stored verdicts (each a [`three_state`](#three_state) below) AND the
"have all four been reported, and who gets to act on that" logic in one place, guarded by a
single `std::mutex` so no cross-field memory-ordering has to be reasoned about by any caller. See
[Document lifecycle, in order](#22-document-lifecycle-in-order) and [Knowing every segment has
been stored, before `on_doc_close()`](#232-knowing-every-segment-has-been-stored-before-on_doc_close)
above for how the four facts map onto the hooks a caller actually sees fire.

Move-only (via a lock-then-snapshot pattern in the move constructor, since reading another
instance's fields must happen under that instance's own lock); not copyable.

<a id="three_state"></a>`three_state` (`enum class three_state : std::uint8_t`) is the three-way
verdict each of the four facts is stored as: `unknown` (not yet reported -- the only valid initial
state), `valid` (reported, positive), `invalid` (reported, negative) -- there is no path back from
`valid`/`invalid` to `unknown`.

**Attributes:**

| Member      | Type                 | Meaning                                                                                                                                                                                           |
| ----------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `syntax_`   | `three_state`        | set by `set_syntax()`; reported by C (the cutter)                                                                                                                                                 |
| `valid_`    | `three_state`        | set by `set_valid()`; reported by V, or by C in folded `cut_with_validation=true` mode                                                                                                            |
| `semantic_` | `three_state`        | set by `set_semantic()`; reported by the worker orchestrating `on_doc_sem_check()`                                                                                                                |
| `stored_`   | `three_state`        | set by `set_stored()`; reported by `pipeline::record_segments_stored()` -- can only ever reach `valid`, never `invalid` (no failure verdict of its own, see `set_stored()` below)                 |
| `done_`     | `int`                | count of facts whose outcome can no longer change the final verdict; reaches `k_done_threshold` (4) via either four individual valid reports or one invalid report short-circuiting straight to 4 |
| `closing_`  | `bool`               | one-shot latch: `true` once `try_start_closing()` has handed out its single `true` answer                                                                                                         |
| `mtx_`      | `mutable std::mutex` | guards all six members above                                                                                                                                                                      |

**Methods:**

| Signature                                                                                                                              | Semantics                                                                                                                                                                                            |
| -------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `[[nodiscard]] bool set_syntax(bool ok) noexcept`                                                                                      | records the syntax verdict, exactly once; returns `true` iff THIS call must go on to call `hooks.on_doc_close()`                                                                                     |
| `[[nodiscard]] bool set_valid(bool ok) noexcept`                                                                                       | records the validation verdict, exactly once (only called when not folded into `set_syntax()`); same return-value meaning                                                                            |
| `[[nodiscard]] bool set_semantic(bool ok) noexcept`                                                                                    | records `on_doc_sem_check()`'s own bool verdict, exactly once; same return-value meaning                                                                                                             |
| `[[nodiscard]] bool set_stored() noexcept`                                                                                             | records that this document's segments have all been written out, exactly once; no `ok` parameter -- always marks `stored_` `valid` (see the member table above); same return-value meaning           |
| `[[nodiscard]] bool is_finished() const noexcept`                                                                                      | `true` once `done_ >= k_done_threshold`, i.e. all four facts are known or the outcome is already decided by a single invalid report; monotonic                                                       |
| `[[nodiscard]] bool try_start_closing() noexcept`                                                                                      | the one-shot "who calls `hooks.on_doc_close()`" decision; returns `true` to exactly one caller, ever, for a given instance -- every `set_*()` call above invokes this internally under the same lock |
| `[[nodiscard]] three_state status() const noexcept`                                                                                    | aggregate across `syntax_`/`valid_`/`semantic_` only (never `stored_`): `invalid` if any of the three is invalid, `valid` iff all three are valid, `unknown` otherwise                               |
| `[[nodiscard]] bool ok() const noexcept`                                                                                               | `status() == three_state::valid`                                                                                                                                                                     |
| `[[nodiscard]] three_state syntax_status() const noexcept` / `valid_status()` / `semantic_status()` / `stored_status() const noexcept` | the individual field's current `three_state`, unaggregated                                                                                                                                           |
