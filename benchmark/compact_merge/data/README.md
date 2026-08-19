# Prepared Cohere10M Data

Generated data is ignored by Git. The preparation script should create:

```text
data/
  manifest.json
  base_pool/
    fp32.*
    int8.*
    ids.*
  tail_pool/
    fp32.*
    int8.*
    ids.*
  queries/
    fp32.*
    int8.*
    ids.*
  ground_truth/
    topk.*
  checksums.sha256
```

`manifest.json` records the source paths/checksums, parser version, selection seed, dimension, dtype, counts, ID ranges,
output files, and output checksums. Base sizes and tail sizes should be represented as prefix counts/offsets into the
disjoint 1M-record pools rather than duplicated payload files where possible.
