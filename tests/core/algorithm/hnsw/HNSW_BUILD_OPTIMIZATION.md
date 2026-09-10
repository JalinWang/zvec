# HNSW FP32 构图优化：本地验证记录

日期：2026-09-07。原始代码基线：`44d8bc6f7c3d656efde2641c9410432ea14c1657`。

## 保留的改动

- 正向剪枝按需获取当前候选，缓存已选邻居的向量；反向剪枝一次获取候选向量。
  `MemoryBlock` 在本次剪枝结束时释放，只在 worker context 中保留数组容量。
- 反向连边的中心到邻居距离使用显式的 stored-vector batch 接口，不改写当前插入 query。
  未支持该接口的 metric 仍使用原有的逐对距离，保留输入方向和扫描计数。
- 默认 IP 构图使用 `MipsSquaredEuclidean` 的 localized spherical injection。
  新增融合的 FP32 AVX512 / AVX2 批量 kernel，一遍读取同时计算内积和候选范数，
  中心范数每批计算一次；其他 CPU 使用原有逐对分派。
- 保留原有剪枝顺序和逐对提前退出，保留流式插入和既有并行构图方式。
  索引格式和 M / efConstruction 没有改变。

## 测量方式

- Windows 本地，Intel Xeon 6982P-C，进程可用 16 个逻辑 CPU；MSVC 14.44，Release。
- FP32，1024 维，M=50（level 0 最大度数 100），efConstruction=500，efSearch=300。
- 新建 MMap HNSW，未量化；固定随机种子 `20260907` 的标准正态数据。
- 每轮生成 100 个独立 query，使用对应 metric 的精确暴力搜索计算 recall@10。
- 构图计时包括创建 worker/context、`add_with_id_impl` 和等待 worker 结束；flush 单独计时。
- 2 万条场景按“原版、优化版”交替运行三轮。5000 条场景每个版本连续运行三轮。
- 向量数据相同，但 HNSW 层级随机数及并发插入顺序没有固定，因此图结构不保证相同。

**这是 compact 使用的 HNSW 插入构图阶段基准，不是完整 Collection Optimize 基准。**
没有包含 reducer 的读取/入队、标量索引、segment 发布等开销，也没有测已有图复用场景。

## 结果

构图耗时取三轮中位数：

| 数据量 | Metric | 线程 | 原版 / 秒 | 优化版 / 秒 | 耗时减少 |
|---:|---|---:|---:|---:|---:|
| 20000 | 默认 IP | 16 | 33.485 | 27.495 | 17.9% |
| 5000 | 默认 IP | 1 | 7.251 | 6.483 | 10.6% |
| 5000 | 默认 IP | 8 | 0.881 | 0.821 | 6.8% |
| 5000 | L2 | 1 | 6.788 | 6.786 | 约 0% |
| 5000 | L2 | 8 | 0.914 | 0.862 | 5.7% |

2 万条、默认 IP、16 线程的原始测量：

| 轮次 | 原版构图 / 秒 | 优化版构图 / 秒 | 原版 recall@10 | 优化版 recall@10 |
|---:|---:|---:|---:|---:|
| 1 | 33.666115 | 27.120973 | 95.9% | 95.8% |
| 2 | 33.484670 | 27.494562 | 96.0% | 96.0% |
| 3 | 33.202989 | 27.778809 | 95.9% | 95.8% |

recall@10 均值为 95.93% → 95.87%，差异约 -0.067 个百分点；
单 query 搜索耗时中位数为 3.064 ms → 3.003 ms；flush 中位数均约 0.085 秒。
5000 条场景中，IP recall@10 均为 100%，L2 均为 99.9%。
这些是小样本合成数据结果，不能据此保证其他数据分布的召回率或相同加速比。
批量 SIMD 与原逐对计算的浮点求和顺序不同，可能影响临界候选的选择。

没有保留两个实测不合适的方案：组合已有批量内积与独立范数 kernel（默认 IP 变慢），
以及每 4 个比较一批的剪枝（额外计算抵消收益，L2 变慢）。单独反向缓存的收益不明显；
最终收益来自融合 MIPS kernel 和正反向向量缓存的组合。

## 复现

在 VS Developer PowerShell 中复用本地 Release 编译目录：

```powershell
cmake -S . -B build_/r
cmake --build build_/r --target hnsw_build_benchmark_test -j 12
$env:HNSW_BENCH_COUNT = '20000'
$env:HNSW_BENCH_THREADS = '16'
$env:HNSW_BENCH_METRIC = 'MipsSquaredEuclidean'
& .\build_\r\bin\hnsw_build_benchmark_test.exe `
  --gtest_filter=HnswBuildBenchmark.DISABLED_Fp32_1024 `
  --gtest_also_run_disabled_tests --gtest_repeat=3
```

L2 使用 `HNSW_BENCH_METRIC=SquaredEuclidean`。该基准默认禁用，不增加日常单测耗时。
对比基线时，在原始代码上使用同一个 benchmark 源文件和相同编译选项，
分别保留两版可执行文件及其 DLL，在同一台机器上交替运行。

## 正确性检查

- `mips_distance_batch_test`：native / 禁用 AVX512 / 禁用批量 AVX2 的分派路径，
  非对齐输入、维度尾部、批次尾部、零向量，与 double 参考计算对比。
- `hnsw_dist_calculator_test`：stored batch 与逐对计算一致性、保留当前 query、
  metric 重绑定、非对称 fallback 的输入方向、空批次、缺失向量和 FP16 fallback。
- `hnsw_streamer_test`：50 项通过，1 项现有 Windows 平台跳过；覆盖 provider、
  不同距离空间、量化、并发写入/查询和 contiguous 存储。
- `hnsw_streamer_buffer_test`：4 项通过，覆盖 BufferPool 存储路径。
- `vector_column_indexer_test`：General / Merge / CosineMerge / Score 共 4 项通过。

以上共 67 项通过，1 项现有平台跳过；`git diff --check` 通过。
