# Run Output Layout

Generated runs are ignored by Git and retained in durable benchmark storage. Each attempt has a globally unique
`run_id`:

```text
runs/
  index.jsonl
  <run_id>/
    manifest.json
    environment.json
    case.json
    status.json
    raw/
      stdout.log
      stderr.log
      zvec.log
      phase_events.jsonl
      process_samples.csv
      proc_io_samples.csv
      pidstat.log
      iostat.log
      block_device_samples.csv
      filesystem_samples.csv
      cloud_disk_metrics.json
      query_events.jsonl
    snapshots/
      collection_empty.tsv
      base_flat.tsv
      initial_build_peak.tsv
      base_indexed.tsv
      tail_persisted.tsv
      compact_peak.tsv
      compact_published.tsv
      reopened.tsv
    metrics/
      phases.json
      process.json
      memory.json
      io.json
      collection_space.json
      query.json
    verification/
      correctness.json
      recall.json
      checksums.sha256
    summary/
      result.json
      result.csv
```

## Retention rules

- `raw/` and `snapshots/` are immutable after the attempt completes.
- `metrics/` and `summary/` are reproducible derivatives and record the parser commit/version used to create them.
- `status.json` is written atomically and contains start/end timestamps, exit status, terminal taxonomy, and paths to
  failure evidence.
- `index.jsonl` contains one append-only catalog record per run.
- Failed, unsupported, OOM, and timeout runs retain the same evidence as successful runs.
- Large collection artifacts may be removed after verification, but their file snapshots, checksums, logical/allocated
  sizes, and cleanup decision remain.
- Raw logs and metric samples must be copied to durable storage before the ECS is released.

Use synchronized monotonic and wall-clock timestamps in every sample stream so phase, process, block-device, cloud-disk,
collection-size, and query metrics can be aligned later.
