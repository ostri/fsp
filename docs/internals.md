# Internals

## Table of contents

1. [Segment cache (`seg_cache_size`)](#1-segment-cache-seg_cache_size)
   1. [Capacity and blocks](#11-capacity-and-blocks)
   2. [Lower bound and the deadlock below it](#12-lower-bound-and-the-deadlock-below-it)
   3. [Cutter wait logging](#13-cutter-wait-logging)
   4. [Import time against cache size](#14-import-time-against-cache-size)
2. [Xerces `getGrammar()` in the cutter](#2-xerces-getgrammar-in-the-cutter)

## 1. Segment cache (`seg_cache_size`)

### 1.1. Capacity and blocks

- `importer_config::seg_cache_size` (default 4194304) is the
  capacity of `segment_pool`'s `blocked_vector<segment_slot>`.
- `segment_pool::init()` rounds it up to whole blocks.
  `sizeof(segment_slot)` is 200 bytes, the block is the largest
  power of two of slots that fits 32 KiB, so
  `block_size()` is 128 (not 4096).
- Blocks are allocated when their first slot is fetched.

### 1.2. Lower bound and the deadlock below it

- A P-role thread keeps the slot index of every processed
  segment in a local batch and releases the slots only when
  `ok_block_flush_size` (or `nak_block_flush_size`) is reached,
  or when the thread ends.
- With a cache smaller than what the P-role threads hold in
  their batches, the cutter waits in `acquire_slot()`, the
  P-role threads wait in `lock_queue::pop()` for ready segments
  and nobody releases a slot. Seen with `clearing.seg-cache-size`
  1000 and `ok_block_flush_size` 4096: 0 rows imported.
- `pipeline::process_files()` raises a smaller cache to
  `importer_config::min_seg_cache(num_parallel)`, which is
  `num_parallel * (ok_block_flush_size + nak_block_flush_size)`,
  and logs a warning.
- Measured with the test "a segment cache smaller than the
  document's segment count still completes" (2 workers,
  `ok_block_flush_size = nak_block_flush_size = 256`, so the
  bound is 1024), cache size forced from the environment, three
  runs each:

  | cache | result |
  | --- | --- |
  | 1024, 896, 640, 512 | completes |
  | 384, 256, 128 | hangs (timeout), 3 of 3 |

- At 384 the process uses no CPU. The cutter is in
  `acquire_slot()` -> `lock_queue::pop()`, a P-role thread is in
  `lock_queue::pop()` on the empty ready queue.
- The real threshold is about `workers * ok_block_flush_size`.
  The bound also counts `nak_block_flush_size`, because
  failed-segment batches hold slots too; with `ok = nak` as in
  this test the bound is twice the real threshold, in ach
  (4096 and 128) it is about 3% above it.

### 1.3. Cutter wait logging

- `acquire_slot()` tries a non-blocking acquire first
  (`try_acquire_slot()`), logs a warning and then blocks.
- Every wait is logged. At the lowest allowed cache size in
  ach (59136 slots) a 10M-transaction import had between 8193
  and 12288 waits in about 117 s.

### 1.4. Import time against cache size

`tool import ct-in`, 10M transactions, 14 workers, profile
build, one run per size, about +-10% noise. Measured before the
lower bound existed:

| slots | import |
| --- | --- |
| 8388608 | 92 s |
| 4194304 | 92 s |
| 2097152 | 95 s |
| 1048576 | 96 s |
| 524288 | 101 s |
| 262144 | 123 s |
| 131072 | 113 s |
| 32768 | 124 s |

## 2. Xerces `getGrammar()` in the cutter

- In a validating parse (`SGXMLScanner`, locked grammar pool,
  `fgXercesUseCachedGrammarInParse`) `GrammarResolver::getGrammar`
  is 17% of the parse time (10% of the whole import).
- It is called from `scanStartTag()` through `switchGrammar()`.
  On a 10618-element sample 6610 calls were counted, always with
  the document's own namespace URI. 6409 elements are the first
  child of their parent and 200 are `CdtTrfTxInf`; this is a
  count match, the cause was not found.
- No scanner feature or property changes it: `IGXMLScanner`
  makes the same 6610 calls and takes the same time.
- Experiment (Xerces 3.3.0 built locally, not part of fsp): a
  one-entry cache of (resolver, URI pointer) -> grammar in
  `switchGrammar()` lowered the parse of an 85 MB document from
  0.95 s to 0.80 s (same compiler flags for both builds). The
  full import time did not change, the write rate during the
  C phase is the limit there.
