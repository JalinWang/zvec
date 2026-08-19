# Compact / Merge Benchmark Plan

## 1. Purpose

This first round establishes the no-delete performance baseline for both the initial index build and the later
compact/merge path. It answers:

1. How do initial-build and compact latency scale with base and tail size?
2. Which index paths have high peak memory or OOM risk?
3. Are the limiting resources CPU, memory, or ECS cloud-disk IOPS/throughput?
4. How much temporary and final collection disk space is required?
5. Does Optimize degrade concurrent query latency, throughput, or recall?

Deletion-triggered rebuilds are deliberately excluded and will be evaluated in a separate round.

## 2. Workload lifecycle

Each case models an indexed immutable base followed by a persisted FLAT tail:

```text
create collection
  -> ingest and persist base as FLAT
  -> monitor initial Optimize / CreateVectorIndexTask
  -> verify and snapshot indexed base
  -> ingest and persist one FLAT tail segment
  -> monitor CompactTask / Optimize
  -> reopen, verify, and run post-compact queries
```

The runner must distinguish these windows instead of reporting one combined time:

| Window                           | Included in primary results | Purpose                                        |
|----------------------------------|----------------------------:|------------------------------------------------|
| Base ingest and FLAT persistence |             Yes, separately | Establish data-preparation cost and disk state |
| Initial target-index build       |                         Yes | Potential optimization target in its own right |
| Tail ingest and FLAT persistence |             Yes, separately | Establish the pre-compact state                |
| Compact / merge                  |                         Yes | Measure base reuse or full rebuild             |
| Reopen and correctness checks    |             Yes, separately | Detect invalid or incomplete outputs           |

For FLAT, an initial `CreateVectorIndexTask` may be a no-op when the segment is already index-ready. The result must say
`NO_OP_ALREADY_READY`; it must not be presented as a microsecond-scale full build.

## 3. Base x tail matrix

Both base and tail use 10K, 100K, and 1M documents. All nine combinations are required:

| Base / Tail |  10K |   100K |      1M |
|------------:|-----:|-------:|--------:|
|         10K | 100% | 1,000% | 10,000% |
|        100K |  10% |   100% |  1,000% |
|          1M |   1% |    10% |    100% |

The percentage is `tail_docs / base_docs`. The lower triangle represents the main large-base/small-tail scenario. The
diagonal and upper triangle locate the point where appending to a small base may cease to be preferable to rebuilding.

The primary matrix uses exactly one tail segment so that tail volume and source fanout are not confounded. A follow-up
topology suite fixes total tail size at 1M and compares:

```text
1 x 1M
10 x 100K
100 x 10K
```

## 4. Indexes and data/quantization modes

Indexes in scope:

- FLAT
- HNSW
- IVF
- Vamana
- DiskANN
- HNSW_RABITQ
- IVF_RABITQ

The three non-RaBitQ modes are all mandatory:

| Mode         | Input `DataType` | `QuantizeType` | Meaning                                       |
|--------------|------------------|----------------|-----------------------------------------------|
| FP32 Raw     | `VECTOR_FP32`    | `UNDEFINED`    | Raw FP32 input and index                      |
| INT8 Raw     | `VECTOR_INT8`    | `UNDEFINED`    | Input vectors are already INT8                |
| FP32 -> INT8 | `VECTOR_FP32`    | `INT8`         | FP32 input with an INT8 quantized index block |

`VECTOR_INT8` and `QuantizeType::INT8` are different workloads and must never share one label in results.

RaBitQ is tested only with FP32 input:

| Index       | Input `DataType` | Effective quantization |
|-------------|------------------|------------------------|
| HNSW_RABITQ | `VECTOR_FP32`    | Dedicated RaBitQ path  |
| IVF_RABITQ  | `VECTOR_FP32`    | Dedicated RaBitQ path  |

RaBitQ cases do not run `VECTOR_INT8` or `FP32 -> INT8`. The ECS must pass the platform and CPU-feature checks before
these cases are scheduled.

Use dimension 128 and L2 for the first round unless the Cohere10M source format requires a different dimension. If its
native dimension is used, it must be identical across every compatible case and remain within the RaBitQ-supported
range.

Before the full matrix, run a 1K-document capability probe for every proposed index/mode pair:

```text
schema validate -> build -> compact -> reopen -> query
```

Unsupported combinations are recorded as `SKIPPED_UNSUPPORTED`, with the exact validation or platform error retained.
They are not failures and are not reported as zero-time runs.

## 5. Expected case count

After capability probing, the maximum matrix contains:

```text
5 regular indexes x 3 mandatory modes x 9 sizes = 135 cases
2 RaBitQ indexes  x 1 FP32 mode       x 9 sizes =  18 cases
                                                   ---------
                                                   153 cases/repetition
```

Three measured repetitions produce up to 459 runs. Run the complete matrix once as a screening pass before committing to
all repetitions. This exposes OOM, unsupported, timeout, and excessively long cases and gives a realistic total runtime
estimate.

## 6. Dataset preparation: Cohere10M

Cohere10M is the sole data source for the first round. A dedicated parser/split script will live at:

```text
benchmark/compact_merge/scripts/prepare_cohere10m.py
```

The script is a required harness deliverable, but is implemented after the source file format and local path are
confirmed. It must:

1. Stream-parse the source rather than load 10M vectors into RAM.
2. Validate IDs, dimension, dtype, finite FP32 values, and INT8 ranges.
3. Verify FP32 and INT8 records are aligned by source ID when both are provided.
4. Select data deterministically using an explicit seed and stable ordering.
5. Reserve disjoint pools so all nine cases reuse identical content:
    - base pool: first 1M selected records;
    - tail pool: next 1M selected records;
    - query/ground-truth pool: separate records or the dataset's supplied query set.
6. Produce nested base prefixes (10K/100K/1M) and tail prefixes (10K/100K/1M), without duplicating payload when hard
   links or offset manifests are sufficient.
7. Write a manifest containing source checksums, output checksums, record counts, dimension, dtype, split seed, IDs, and
   script commit.
8. Be idempotent: identical inputs and parameters produce identical manifests.

Prepared data layout is documented in [data/README.md](./data/README.md).

## 7. Fixed test parameters

The manifest for every run must record explicit values for:

- dimension, metric, and top-k;
- insert batch size;
- Optimize and build concurrency;
- HNSW M and `ef_construction`;
- IVF nlist, iterations, sample count/ratio, and SOAR setting;
- Vamana degree, search-list size, alpha, and layout flags;
- DiskANN degree, list size, PQ settings, and I/O backend;
- RaBitQ bits, clusters, sample count, and reformer settings;
- zvec commit, build type, compiler, and compile flags.

Production defaults may be selected, but they must be materialized into the case manifest rather than represented by the
word `default`.

Use one primary concurrency setting. Thread sensitivity is a separate sentinel test at base=1M/tail=100K with 1 thread,
the primary thread count, and all cores.

## 8. Controls

The full matrix measures the current implementation. Controls are limited to three sentinel sizes to keep the experiment
bounded:

| Base | Tail | Reason                          |
|-----:|-----:|---------------------------------|
|  10K |  10K | Small equal-size merge          |
|   1M |  10K | Main large-base/small-tail case |
|   1M |   1M | Large equal-size merge          |

- FLAT/HNSW: current reuse versus forced no-reuse.
- IVF/Vamana/DiskANN/RaBitQ: current Compact versus a clean initial build from the combined data.
- Previous reuse-first results remain historical evidence; rerun only sentinel points to detect regressions.

Every case must log the actual task type, whether reuse was attempted, whether it succeeded, and the reason for
fallback.

## 9. Metrics

### 9.1 Phase timing and CPU

Capture separately for initial build and compact:

- wall time;
- user/system CPU time and utilization;
- docs/s using both total docs and newly processed tail docs;
- segment switch, scalar, vector stage 1, vector stage 2, copy, open, merge, train, build, flush, publish, close, and
  reopen durations;
- task type and reuse decision.

Phase timing should be emitted as structured JSONL with a `run_id` and monotonic timestamps. Human-readable logs are
retained but are not the source of truth for aggregation.

### 9.2 Memory

- sampled RSS and peak RSS/VmHWM;
- cgroup `memory.current` and `memory.peak` when available;
- major/minor page faults;
- swap activity;
- allocation failure, OOM kill, or timeout.

Derived metric:

```text
memory_amplification = peak_rss / raw_vector_bytes_in_scope
```

After the full matrix identifies the heaviest cases, replay only those under fixed cgroup memory limits to determine the
minimum viable memory budget.

### 9.3 Collection disk usage

Collection disk usage is a first-class metric for both initial build and compact. Record checkpoints at:

```text
empty collection
after base FLAT persistence
peak during initial build
after initial build and cleanup
after tail FLAT persistence
peak during compact
after compact publication and input cleanup
after reopen
```

At each checkpoint record:

- total logical bytes (`st_size`);
- allocated bytes (`st_blocks * 512`);
- file count;
- filesystem free bytes;
- per-file relative path, logical bytes, allocated bytes, and block type when known.

During build/compact, sample filesystem free space and collection allocated size at a fixed interval. Directory scans
must use metadata only and their overhead must be measured during the smoke pass. If the scan perturbs short cases, use
checkpoint snapshots for short runs and mount-level free-space sampling for the peak.

Derived metrics:

```text
build_temp_space     = peak_build_bytes - pre_build_bytes
compact_temp_space   = peak_compact_bytes - pre_compact_bytes
collection_space_amp = peak_allocated_bytes / final_allocated_bytes
final_bytes_per_doc  = final_allocated_bytes / final_doc_count
```

### 9.4 I/O and ECS cloud disk

Retain three independent layers:

1. Process: `/proc/<pid>/io`, read/write bytes, rchar/wchar, cancelled writes.
2. Block device: read/write IOPS and MiB/s, request size, await, queue depth, utilization, and total bytes.
3. Alibaba Cloud: per-disk read/write IOPS, BPS, latency, IOPS/BPS utilization, and Burst IO.

The environment manifest must include cloud-disk ID, type, capacity, performance level, provisioned IOPS/throughput, and
Burst configuration. Record whether each run enters burst or reaches the instance/disk ceiling.

Derived metrics:

```text
tail_write_amplification   = physical_write_bytes / raw_tail_bytes
output_write_amplification = physical_write_bytes / final_index_bytes
read_amplification         = physical_read_bytes / input_index_bytes
```

Short cases use process/block-device sampling as the primary evidence because cloud-monitor granularity may be too
coarse.

### 9.5 Query interference and quality

Do not run concurrent queries for all 153 cases. Use:

- every supported index/mode at base=1M/tail=10K;
- builder-heavy indexes at base=1M/tail=1M;
- additional HNSW/RaBitQ 1M+1M cases if capacity permits.

Run a stable query load before, during, and after Optimize. Record P50/P95/P99/ P999, actual QPS, timeouts/errors, and
recall@10 against fixed ground truth. Freeze per-index query parameters after calibrating them to the desired baseline
recall.

## 10. Cache and environment control

- Use a dedicated ECS with no unrelated workload.
- Save instance, CPU/NUMA, kernel, filesystem, mount, cloud-disk, and build details in `environment.json`.
- Primary capacity results are cold-cache; selected sentinels also run warm.
- Restore collection state and clear/evict cache outside the measurement window.
- Wait for CPU load and disk queue to return to the recorded idle baseline before starting the next case.
- Keep AutoPL burst disabled for the controlled suite when feasible. If production uses burst, run a separately labeled
  realistic-burst suite.
- Reverse or randomize case order between repetitions so time-of-day and disk drift do not correlate with data size.

## 11. Reproducibility and randomness

Input vectors, split IDs, query set, and ground truth use explicit seeds and checksums. Index parameters and thread
counts are fixed.

Known algorithmic randomness must not be hidden:

- HNSW graph levels start from a stable default `mt19937` sequence, while multi-thread insertion ordering can still
  vary.
- DiskANN PQ sampling currently uses a fixed internal seed.
- Some rotation-based quantizers use `random_device` and may vary between builds.

Policy:

1. Run every valid case three measured times after smoke/screening.
2. Report median, min/max, coefficient of variation, and ratio-of-medians.
3. Expand to five repetitions when duration CV exceeds 5%, I/O CV exceeds 10%, or recall varies by more than 0.5
   percentage points.
4. Reuse one immutable base artifact for performance repetitions after restoring it outside the measurement window.
5. Build three independent bases for representative graph/quantized quality cases to expose index-construction
   randomness.
6. Do not require byte-identical index files. Require count, reopen, correctness, and quality checks to pass.
7. If uncontrolled rotator randomness materially affects results, add an explicit benchmark seed path before drawing
   conclusions.

## 12. Correctness and status taxonomy

Each successful run verifies final count, reopen, sampled fetches, row-ID range, no duplicate/missing sampled documents,
query validity, and recall. FLAT should match exact ground truth.

Use explicit terminal statuses:

```text
SUCCESS
NO_OP_ALREADY_READY
SKIPPED_UNSUPPORTED
OOM
TIMEOUT
BUILD_FAILED
COMPACT_FAILED
REOPEN_FAILED
CORRECTNESS_FAILED
QUERY_QUALITY_REGRESSION
ENVIRONMENT_INVALID
```

## 13. Execution stages

1. **Harness and capability probe**: implement Cohere10M preparation, structured case manifests, monitors, and 1K smoke
   cases.
2. **Single screening pass**: run all valid cases once; estimate duration and identify OOM/timeouts.
3. **Optimize-only confirmation**: run three repetitions without concurrent queries; extend high-variance cases to five.
4. **Query-interference subset**: run representative 1M+10K and heavy 1M+1M cases.
5. **Controls and sensitivity**: no-reuse/full-build controls, thread sensitivity, warm-cache, and tail fanout.
6. **Analysis**: generate heatmaps, phase waterfalls, resource timelines, and per-index recommendations from retained
   raw data.

## 14. Run data and deliverables

Every run uses the layout in [runs/README.md](./runs/README.md). Raw logs and raw samples are immutable. Normalized
metrics and summaries are regenerated from them and include the parser version.

Final reporting includes:

1. 3x3 heatmaps for initial-build time, compact time, peak RSS, physical reads/ writes, and peak collection disk usage.
2. Initial-build and compact phase waterfalls.
3. CPU/RSS/IOPS/BPS/latency/collection-size timelines for sentinel cases.
4. Query P99/QPS/recall before, during, and after Optimize.
5. A compatibility matrix with unsupported reasons.
6. A decision table identifying each index's dominant bottleneck, triggering scale, and recommended next optimization.

The main analysis should fit simple response models such as:

```text
T_build   ~= a * base_docs + b
T_compact ~= c * base_docs + d * tail_docs + e
RSS_peak  ~= f * base_docs + g * tail_docs + h
```

The goal is not a fastest-index ranking. It is to determine whether base work, tail work, memory copies, or cloud-disk
traffic dominates each path, and whether the next investment should be reuse, lower-memory builders, tiered compaction,
or no change.
