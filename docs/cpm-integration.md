# Consuming fsp via CPM

This document explains how to pull `fsp::importer` and `fsp::exporter<T,Q>` (plus their
supporting classes -- `pipeline_hooks`, `cb_exporter<T,Q>`, `transaction_t`/`qualificators_t`,
...) into another project via [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake), the same
package manager fsp itself uses for `fmt`, `spdlog`, `magic_enum` and `ostri/logger` (see the
top-level [CMakeLists.txt](../CMakeLists.txt)).

## 1. Prerequisites

- **XercesC and LibXml2** are resolved via `find_package(... REQUIRED)`, not CPM (see the "Find
  required packages" comment in `CMakeLists.txt` for why) -- install the dev packages yourself,
  same as building fsp standalone (see the main [README.md](../README.md)):
  ```bash
  sudo dnf install -y xerces-c-devel xerces-c-doc.noarch libxml2 libxml2-devel
  ```
- **`fsp::importer` requires GCC's experimental C++26 reflection branch (GCC 16.1.1) and the
  `-freflection` compiler flag.** `fsp::importer`'s constructor takes an `importer_config`
  whose `targets` field is built with `fsp::proc_data_of<^^YourNamespace>()` -- a consteval
  reflection call over your own schema types (see `src/test/work.hpp` for a worked example, and
  `src/test/pacs8.cpp:40` for the call site). Your own project's target that calls
  `proc_data_of<^^...>()` must add `-freflection` itself; there is no way around this for the
  importer.
- **`fsp::exporter<T,Q>` / `fsp::cb_exporter<T,Q>` do NOT need reflection.** They are plain C++26
  class templates (see `src/exporter/exporter.hpp`'s file comment: "Header-only ... there is no
  non-template translation unit"). If you only need document export, you can consume fsp without
  ever passing `-freflection` in your own targets.

## 2. CMakeLists.txt integration

```cmake
include(cmake/CPM.cmake) # or wherever your project keeps its own copy of CPM.cmake

CPMAddPackage(
    NAME fsp
    GITHUB_REPOSITORY <org>/fsp
    GIT_TAG <commit-or-tag>
    OPTIONS
        "FSP_BUILD_EXECUTABLES OFF" # skip fsp's own validate/gen/pacs8/pacs8-cb/validator
        "FSP_BUILD_TESTS OFF"       # skip fsp's own test_pars/t_refl/unit_tests
        "BUILD_TESTING OFF"        # skip fsp's own Catch2 fetch
)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE fsp::fsp_lib)
```

`fsp::fsp_lib` is a namespaced `ALIAS` for the internal `fsp_lib` static-library target -- link
against it exactly like `fmt::fmt` or `XercesC::XercesC`. It publishes its include directories
and its own dependencies (`logger::logger`, `fmt::fmt`, `magic_enum::magic_enum`,
`XercesC::XercesC`, `LibXml2::LibXml2`) as `PUBLIC`, so `my_app` needs no further
`target_include_directories()`/`target_link_libraries()` calls for fsp's own headers or
transitive dependencies.

`FSP_BUILD_EXECUTABLES`/`FSP_BUILD_TESTS`/`BUILD_TESTING` all default to `OFF` automatically when
fsp is pulled in as a subdirectory (CPM/`add_subdirectory()`/`FetchContent`) rather than built
directly -- the `OFF` values above are shown for clarity, not strictly required.

**Both fsp and this project must use the same GCC 16.1.1 `-freflection` toolchain** if you use
`fsp::importer` -- set `CMAKE_CXX_COMPILER` accordingly for the whole build, not just fsp's own
subdirectory.

## 3. Using `fsp::importer`

`fsp::importer` is a public facade over fsp's V/C/P hybrid parsing pipeline (see
`src/importer/importer.hpp`). It never interprets your document's fields itself -- it is
generic over a *reflected schema namespace* you define, and (optionally) a `pipeline_hooks`
subclass you provide for lifecycle callbacks.

### 3.1 Define your schema

Model each segment (a repeatable top-level tag your document splits on) as a class deriving from
`fsp::seg_schema`, annotated with `xpath`-like member attributes. See `src/test/work.hpp` for the
full, working example this is adapted from:

```cpp
// my_schema.hpp
#pragma once
#include "reflection.hpp"

namespace my_ns::my_schema
{
  class [[= "header=/Document/GrpHdr"]] doc_header : public fsp::seg_schema
  {
  public:
    [[= "GrpHdr/MsgId"]]        str_t     msg_id;
    [[= "GrpHdr/NbOfTxs"]]      big_int_t txn_count;
  };

  class [[= "transaction=/Document/CdtTrfTxInf"]] doc_txn : public fsp::seg_schema
  {
  public:
    [[= "CdtTrfTxInf/PmtId/TxId"]] str_t    txn_id;
    [[= "CdtTrfTxInf/Amt"]]        amount_t amount;
  };
} // namespace my_ns::my_schema
```

### 3.2 (Optional) Implement your own `pipeline_hooks`

Derive from `fsp::pipeline_hooks_crtp<Derived>` (see `src/importer/pipeline_hooks.hpp`) to
receive lifecycle callbacks -- `on_run_start`/`on_run_end` on the main thread,
`on_wrk_start`/`on_wrk_end`/`on_doc_open`/`on_doc_close`/`on_seg_proc` on worker threads. One
clone is made per worker thread automatically (via CRTP, no extra code needed), so your own
per-thread state needs no locking. `src/test/pacs8_cb.hpp` / `pacs8_cb.cpp` in this repo is a
complete worked example (logs every hook call); skip this step entirely if you only need
`import_docs()`'s return value -- fsp supplies a no-op `fsp::default_pipeline_hooks` for that.

```cpp
// my_hooks.hpp
#pragma once
#include "pipeline_hooks.hpp"

class my_hooks : public fsp::pipeline_hooks_crtp<my_hooks>
{
public:
  bool on_seg_proc(const fsp::xml_segment& segment,
                   fsp::segment_result&    result,
                   bool                    is_first,
                   bool                    is_last,
                   const logger::Logger&  log) override
  {
    // fsp::materialize_variant<my_ns::my_schema>(result.seg_type(), result) turns result
    // back into your own doc_header/doc_txn type here, if you need field-level access.
    return result.values().complete(); // default semantic verdict
  }
};
```

### 3.3 Run the import

```cpp
#include "importer.hpp"
#include "my_schema.hpp"
#include "my_hooks.hpp" // optional

int main()
{
  auto cfg = fsp::importer_config{
      .targets        = fsp::proc_data_of<^^my_ns::my_schema>(), // requires -freflection
      .num_of_workers = 16U,
      .log_config     = my_logger_config(),
      .program_name   = "my_app",
  };

  auto     importer = fsp::importer(cfg);
  my_hooks hooks;
  auto     res = importer.import_docs({"data.xml"}, "schema.xsd", hooks);
  if (! res)
  {
    // res.error().to_string() describes the failure
    return 1;
  }
  // res->total_docs(), res->total_segments(), importer.get_results(), importer.get_errors(), ...
}
```

Omit the `hooks` argument entirely to use `fsp::default_pipeline_hooks` (a no-op) if you don't
need callbacks -- see `src/test/pacs8.cpp` for that simpler variant.

## 4. Using `fsp::exporter<T,Q>`

`fsp::exporter<T,Q>` runs a maximally parallel export of documents assembled from your own
transaction type `T` and run-qualifiers type `Q`, driving a `cb_exporter<T,Q>` callback you
implement to supply data and receive lifecycle notifications (see
`src/exporter/exporter.hpp`/`cb_exporter.hpp`). No reflection is required.

### 4.1 Define your transaction and qualifiers types

```cpp
#include "exporter_types.hpp"

struct my_txn : fsp::transaction_t
{
  // fsp::transaction_t already provides id (uint128_t), type (int), value (str_t) --
  // add your own fields here if you need more.
};

struct my_qualifiers : fsp::qualificators_t
{
  // fsp::qualificators_t already provides run_id (int) -- add your own fields here.
};
```

### 4.2 Implement your `cb_exporter`

Derive from `fsp::cb_exporter_crtp<Derived, T, Q>` and implement all six pure virtual methods
(see `src/test/test_cb_exporter.cpp`'s `demo_cb` for a minimal, compiling reference):

```cpp
#include "cb_exporter.hpp"

class my_cb : public fsp::cb_exporter_crtp<my_cb, my_txn, my_qualifiers>
{
public:
  fsp::exp_result<fsp::str_t> fetch_doc_name(const my_qualifiers& qualifiers,
                                             fsp::cstr_t          path,
                                             int                  drain_id,
                                             std::size_t          block_number,
                                             std::size_t          total_blocks,
                                             fsp::cstr_t          filename_prefix) override;

  fsp::exp_result<run_stat_t> fetch_run_stat(const my_qualifiers& qualifiers, int drain_id) override;

  fsp::fetch_doc_data_result_t<my_txn> fetch_doc_data(const my_qualifiers& qualifiers,
                                                      int                  drain_id,
                                                      std::uint64_t        doc_id) override;

  fsp::exp_result<fsp::str_t> prepare_transaction(std::size_t   ndx,
                                                  int           drain_id,
                                                  std::uint64_t doc_id,
                                                  const my_txn& data) override;

  fsp::exp_result<fsp::str_t> prepare_header(const my_qualifiers&              qualifiers,
                                             int                               drain_id,
                                             std::uint64_t                    doc_id,
                                             const fsp::txn_block_t<my_txn>&   block) override;

  fsp::exp_result<fsp::str_t> prepare_footer(const my_qualifiers&              qualifiers,
                                             int                               drain_id,
                                             std::uint64_t                    doc_id,
                                             const fsp::txn_block_t<my_txn>&   block) override;

  bool document_prepared(const my_qualifiers& qualifiers, int drain_id, std::uint64_t doc_id) override;
};
```

`clone()` itself needs no implementation -- `cb_exporter_crtp` provides it (copies `*this` via
`my_cb`'s own copy constructor), so `my_cb` must stay copy-constructible and must not share
mutable state across copies: `exporter<T,Q>::execute()` clones your "prototype" instance once
per worker thread, and each clone is used exclusively by its own thread for the run's duration.

### 4.3 Run the export

```cpp
#include "exporter.hpp"

int main()
{
  auto cfg = fsp::exporter_config_t{
      .drain_list        = {{.id = 1, .name = "BANKXX00", .max_doc_txn = 10000}},
      .number_of_threads = 4,
      .filename_prefix   = "export",
      .tmp_dir           = "/var/spool/my_app/tmp",
      .target_dir        = "/var/spool/my_app/out",
      .error_dir         = "/var/spool/my_app/err",
  };
  my_qualifiers qualifiers{.run_id = 1};

  auto exp = fsp::exporter<my_txn, my_qualifiers>(cfg, qualifiers, my_logger, "my_app");

  my_cb proto; // the "prototype" callback instance -- exporter clones it per worker thread
  auto  res = exp.execute(proto);
  if (! res)
  {
    // res.error().to_string() describes the failure
    return 1;
  }
  // res->total_documents, res->total_transactions, res->elapsed_ms
}
```

## 5. Reference implementations in this repository

| What                                   | File                                                              |
|-----------------------------------------|--------------------------------------------------------------------|
| Reflected schema example               | [src/test/work.hpp](../src/test/work.hpp)                          |
| `pipeline_hooks` subclass example       | [src/test/pacs8_cb.hpp](../src/test/pacs8_cb.hpp) / [pacs8_cb.cpp](../src/test/pacs8_cb.cpp) |
| Importer, no custom hooks               | [src/test/pacs8.cpp](../src/test/pacs8.cpp)                         |
| Importer, with custom hooks             | [src/test/pacs8-cb.cpp](../src/test/pacs8-cb.cpp)                   |
| `cb_exporter` subclass example           | [src/test/test_cb_exporter.cpp](../src/test/test_cb_exporter.cpp)  |
