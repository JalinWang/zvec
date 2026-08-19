# Case Configurations

Committed case templates and generated case manifests belong here. A generated case must uniquely identify:

```text
index + data_mode + base_docs + tail_docs + repetition + control_mode
```

Templates must contain explicit index/build/query parameters. Generated case manifests also include dataset checksums,
environment ID, zvec commit, and a stable case ID. Do not encode an implicit `default` whose value can change with the
build.
