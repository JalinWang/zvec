# Harness Scripts

Planned scripts:

```text
prepare_cohere10m.py   # streaming parse, validation, split, checksums
generate_cases.py      # capability-aware case matrix
run_case.py            # lifecycle orchestration and run manifest
monitor_process.py     # RSS, CPU, faults, /proc/<pid>/io
monitor_collection.py  # logical/allocated collection bytes and file snapshots
collect_cloud_disk.py  # ECS/EBS raw cloud-disk metrics
normalize_run.py       # raw logs -> normalized metrics and summary
analyze.py             # tables, heatmaps, timelines, response models
```

Scripts must never overwrite a completed run. A retry receives a new `run_id`
and links to the previous attempt in its manifest.
