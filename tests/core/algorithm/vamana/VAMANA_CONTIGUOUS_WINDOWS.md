# Vamana contiguous 持久化：Windows 本地验证

## 范围与结论边界

2026-09-07，本地 Windows / MSVC Release，dense FP32 1024 维，IP
(`MipsSquaredEuclidean` 构图、query metric 检索)。沿用当前工作区，包括先前的
MIPS batch kernel 优化；三种布局都使用相同的 kernel 和同一张图。
没有使用远端 Linux，没有修改系统大页策略。

实验代码是 `vamana_layout_benchmark_test.cc` 中显式禁用的测试，不改变生产文件格式。
仅用于验证 FP32 的布局转换与查询可行性，不能作为生产快照 reader 使用。

## Windows 大页语义

`MemoryHelper::AllocateHugePage` 的 Windows 分支实际使用 `_aligned_malloc`，
按普通页对齐、按 2 MiB 向上取整容量。没有调用 `MEM_LARGE_PAGES`；容量取整不等于
使用大页。当前 contiguous 因而能在 Windows 普通页条件下运行，不依赖 Linux THP。

Windows 显式大页需要 `SeLockMemoryPrivilege`、`GetLargePageMinimum` 和
`VirtualAlloc(... MEM_LARGE_PAGES ...)`，属于不可换出的内存。
见 [Microsoft Large-Page Support](https://learn.microsoft.com/en-us/windows/win32/memory/large-page-support)。

文件支持的 mapping 不能使用 `SEC_LARGE_PAGES`；该标志仅支持 pagefile mapping。
见 [Microsoft 对文件映射大页限制的说明](https://devblogs.microsoft.com/oldnewthing/20250409-00/?p=111061)。
因此 Windows 上应将普通页文件 mmap 作为直接持久化的基线。如未来需要显式大页，
需另设“大页内存分配 + 文件读取/拷贝”的加载策略，无法同时承诺文件零拷贝映射。
本实验没有测显式大页，也不能外推为 Linux THP on/off 的对照实验。

## 当前路径的限制

- `VamanaContiguousStreamerEntity::build_contiguous_memory()` 在 open 后读取 chunk，
  拆出 vector body、可选 extra values 和 `[key, degree, adjacency]`，向量 stride 按 64B 对齐。
  原始 chunks 仍保留；contiguous 只是额外内存副本。
- 插入会调用 `degrade_to_mmap()`，依赖原始 chunks 恢复写入；只持久化 contiguous 后不能
  原样复用这条路径。邻接更新目前也同时写 chunk 和 contiguous graph。
- DB `SegmentHelper::MergeWithOptionalReuse` 最后调用 `Flush()`，并未调用 streamer dump。
  `Index::merge` 用 `MixedStreamerReducer` 重放向量，Vamana 可在其后运行 two-pass finalize。
- 现有 `VamanaStreamer::dump()` 会选择 medoid、写 `VamanaSearcher` 元数据；
  `VamanaEntity::dump_segments()` 会重排 ID 并写 features/neighbors/mapping/distances。
  仓库里没有对应 `VamanaSearcher` 注册/实现。这个文件格式也不是现有 ChunkBroker open
  或当前 contiguous 内存的直接镜像。不能只换一个调用就完成持久化接入。

## 实验设计

固定随机种子 20260907，单线程构建一张图（R=64、L=100、alpha 默认 1.2），
使用 200 条独立随机 query、top10、ef=200。

比较：

1. 原始 chunk mmap。
2. 现有 contiguous 普通页内存副本。
3. 测试原型：从原始 chunk 顺序导出向量区与固定宽度 graph 区，关闭 writer 并调用
   `FlushFileBuffers`，随后只读 `CreateFileMapping/MapViewOfFile`，直接绑定生产 contiguous
   查询模板。没有重新插入或重新计算边。

逐条比较全部向量字节、key、degree 和边；查询比较每个结果的 ID 与 score 完全一致。
各布局预热后，轮换顺序测五轮，每轮 200 query，计时包含结果转换。
测试保留三个布局以便同图对照，未将进程 RSS 当作各方案的独立内存占用。
map 计时只包含建立映射，不含全文件缺页加载；结果是热缓存查询，不是冷启动性能。

## 实测结果

20,000 条向量，同一张图，三个独立进程运行，每进程每布局五轮，共 15 个批次均值。
下表为这些批次均值的中位数（不是单 query 延迟分位数）。

| 布局 | ms/query | 相对 chunk mmap 耗时降低 |
| --- | ---: | ---: |
| chunk mmap | 2.434832 | — |
| contiguous 普通页内存 | 2.270196 | 6.76% |
| contiguous 普通页文件 mmap 原型 | 2.285016 | 6.15% |

所有运行的查询 ID 和 score 完全一致，全部 20,000 个记录及邻接边逐项一致。
说明本样本没有因纯布局转换改变搜索结果；这不是独立测量的绝对 recall 数值。
首个进程的收益更高，重复测量后收敛到约 6–7%，应采用合并结果而非首轮结果。

- 原 chunk → contiguous 内存转换中位数：**32.808 ms**。
- chunk → snapshot 写文件并 `FlushFileBuffers` 中位数：**191.779 ms**。
- snapshot 建立只读映射中位数：**0.081 ms**；不含缺页及校验扫描。
- 文件大小：原 streaming 文件 **95,784,960 B**；原型 snapshot **87,284,160 B**。
  原型省略 neighbor distances、通用元数据与 key 反查索引，不能把这个差值全部归因于布局压缩。
- 构图并 flush：单线程 **257.35 s**。这是建基准图的成本，不是多线程 Optimize 对照。
  正式转换无需再构图，但如果本来就需要合并/重构图，最终导出只增加 O(N·D + N·R) 数据搬运。
- 现有 `vamana_streamer_test` **16/16 通过**，新布局测试 **1/1 通过**并重复三次。
  合计 17 个不同测试；验证日志位于 `build_/vamana_streamer_validation.log` 和
  `build_/vamana_layout_20k_final*.log`。

原型首次运行在入口点距离计算处失败：`VamanaDistCalculator::batch_dist(id)` 调用
虚函数 `get_vector(id)`，继承实现仍查原始 chunks。原型补齐该虚函数后通过全部验证。
这直接验证了“不能只绑定 typed 热路径指针，还必须补全 reader 接口”的接入要求。

结论：**无需大页也有可测收益，直接持久化不可变 contiguous 图在 Windows 上可行**。
本实验支持继续实现 reader/dumper 与 Optimize 最终发布阶段的接入，但尚未实现生产接入，
也没有给出完整 Optimize 的加速比。数据只有约 87 MB，不能外推到超缓存、内存压力、
冷启动或更大规模；后续应在相同 recall 下测业务向量和端到端 compact。

## Optimize 方案建议

可行的目标是“**完整图构建/合并完成后，流式导出不可变 contiguous 快照**”。
单个完整图、无删除、无量化变化且 ID 保持一致时，可完全保留拓扑而省去重新插入。
多 segment 的各自图不能仅拼接：缺少跨图连接；物理删除节点也不能只删边后宣称 recall 不变。
若保留被删除节点参与导航，则需保留向量并用结果过滤，不能同时承诺物理回收。

建议接入顺序：先保留现有 merge/finalize，新增最终快照导出与只读 reader；
之后单独优化满足条件的单图转换。构建时间占主导的多 segment compact 不会仅因布局落盘消失。

正式实现需包含：

- 格式 magic/version、metric/converter 元信息、维度/类型/stride/offset/length、checksum、
  文件尺寸溢出和邻接 ID 边界校验；extras 与量化元数据独立保留，64B 对齐。
- node ID、key、entry point、删除过滤及 DB row ID 重写一致；如重排节点需同步重写边。
- 完整读取接口：search、fetch_vector、get_vector_by_key、provider/iterator、BF 和下一次 merge。
  当前原型仅证明生产 ANN 查询模板可直接读取映射布局。
- 不可变段 + 可写增量段，或显式恢复 streaming 格式；不能沿用没有源 chunks 的 degrade。
- 暂存文件、完整校验与 flush 后再通过现有 segment/manifest 提交机制发布，保留旧格式兼容。
  Windows mapping 生命周期内的文件替换/删除需与 reader 释放和版本回收协调。
- 若需后续图 refinement，保留/恢复 neighbor distance 数据；只读快照可选择不存，但需明确能力。

## 复现

在 VS x64 开发环境，复用 `build_/r`：

```powershell
cmake --build build_/r --target vamana_layout_benchmark_test vamana_streamer_test -j 12
$env:VAMANA_BENCH_COUNT = '20000'
./build_/r/bin/vamana_layout_benchmark_test.exe --gtest_also_run_disabled_tests
./build_/r/bin/vamana_streamer_test.exe
```

新增测试文件后需先重新运行 CMake configure，使 GLOB 发现文件。
可设置 `VAMANA_BENCH_KEEP=1` 保留测试生成的文件，后续设置 `VAMANA_BENCH_REUSE=1`
复用该图以免再次构建；COUNT 必须一致。此选项仅用于本测试生成的 fixture。
