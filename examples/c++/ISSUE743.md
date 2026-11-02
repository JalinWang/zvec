# Issue #743：原生 C++ optimize 排查

2026-09-10，问题：https://github.com/alibaba/zvec/issues/743

已绕过 Node.js，在 Windows 和 Linux 上复现单文档标量集合的 optimize 固定开销。
直接调用 `Collection::optimize(OptimizeOptions{concurrency})` 即可；C API 的
`zvec_collection_optimize()` 也直接转调同一方法，但目前不提供 concurrency 参数。

## 复现程序

`issue743_optimize.cc`：STRING `file_id`，非 nullable，默认 INVERT；upsert 一条
`one / alpha.ts`，单独计时 create、upsert、optimize、fetch、再次 optimize、close、cleanup。
检查写入状态和 fetch 的实际字段值。每种 concurrency 测 5 次，交替执行 0/1，每次新建目录。
第二次 optimize 用来测量没有新写入的情况，因此 close 数值是在第二次 optimize 后测得。
无插桩版本用于总耗时，插桩版本仅用于分解。

运行方式（父目录须存在，NEW_ROOT 必须不存在）：

```text
issue743_optimize NEW_ROOT 5 1 1    # issue 默认配置
issue743_optimize NEW_ROOT 5 1 0    # INVERT 保留，关闭 range optimization
issue743_optimize NEW_ROOT 5 0      # 去掉 INVERT，消融对照
```

本机已编译程序：`build_/r/bin/issue743_optimize.exe`。
本机复用 Release 编译参数和链接依赖的脚本：`build_/r/build_issue743.bat`。
先执行 `cmake --build build_/r --target zvec -j 14`，再执行该脚本即可重新链接。
该脚本需要 VS 2022 BuildTools（环境初始化参照 build_ninja.bat）。

Linux 对已有匹配源码的共享库，可以直接编译：

```sh
g++-12 -O2 -std=c++17 examples/c++/issue743_optimize.cc \
  -Isrc/include -Ibuild/external/usr/local/include \
  -Lbuild/lib -Wl,-rpath,"$PWD/build/lib" -lzvec -o issue743_optimize
```

本次为消除数据库源码差异，另在 `/tmp/zvec-issue743-44d8bc6` 隔离工作树编译了
Windows 提交 `44d8bc6f7c3d656efde2641c9410432ea14c1657` 的全部 114 个数据库源文件，
复用远端已有的 core/ailego/第三方静态库。最终 Linux 程序在该工作树的
`issue743-build/issue743_optimize`。编译脚本保存在本机
`build_/r/issue743_profile/linux_build.py`，阶段插桩脚本为 `linux_profile.py`。
没有修改远端原工作区或生产源码。

## 结果

均为每组 5 次的中位数，单位 ms。

| 配置 | Windows c=0 | Windows c=1 | Linux ext4 c=0 | Linux ext4 c=1 |
| --- | ---: | ---: | ---: | ---: |
| 默认 INVERT optimize | 61.611 | 60.190 | 25.617 | 25.112 |
| INVERT，range optimization=false | 36.141 | 36.057 | 13.966 | 14.124 |
| 不建 INVERT | 5.732 | 6.090 | 1.577 | 1.460 |
| 默认 INVERT，再次 optimize，无新写入 | 0.430 | 0.410 | 0.021 | 0.018 |

默认配置在本次环境约相差 2.4 倍，复现了方向，未复现 issue 的 191.83 ms 绝对值或 7.4 倍倍率。
Windows 为 NT 10.0.26100、MSVC 19.44 /O2 Release；Linux 为内核 7.0.0-22、GCC 12 /O3 Release。
两端均报告 Xeon 6982P-C、16 logical CPUs，但仍是不同环境，磁盘和负载没有控制。
Windows 保留了工作区已有的向量算法改动；本案例没有向量字段，不走这些算法。
这不是完整二进制/编译器完全一致的 OS 隔离实验，也不是对 npm 0.7.0 发布包的重新构建。

补充：远端原有版本在 tmpfs `/tmp` 上约 9 ms，在 ext4 上约 25 ms。
这组存储对照来自远端原有提交 `dee05a0`，不混入上面的同数据库源码结果。

## 耗时定位

函数级计时中位数如下，存在包含关系，不能相加：

| 步骤 | Windows ms | Linux ext4 ms |
| --- | ---: | ---: |
| switch_to_new_segment_for_writing | 60.713 | 25.225 |
| 旧 Segment::dump，含 flush 和 seal | 32.552 | 14.726 |
| InvertedIndexer::seal | 24.642 | 11.105 |
| generate_statistical_indexes | 17.987 | 7.703 |
| 每次 RocksdbContext::reset_cf（每次 optimize 两次） | 8.959 | 3.816 |
| RocksdbContext::compact | 2.393 | 1.381 |
| execute_compact_task | 0.001 | 0.002 |

除 dump 外，switch 还会立即创建新写入 segment，包括新的 RocksDB 和 column families：
Windows 约 27–28 ms，Linux 约 10–11 ms。这解释了 concurrency=1 基本无效：
主耗时在切换/封存阶段，不在后面的向量构建任务。

Windows 诊断版还包装了静态链接代码调用的 Win32 导入，调用真实系统 API 并计时，
仅在第一次 optimize 区间启用。10 次均为：50 次 FlushFileBuffers、64 次 CreateFileW、
109 次 CloseHandle。FlushFileBuffers 累计约 19–23 ms，CreateFileW 约 5.6–6.0 ms。
这是跨线程调用耗时总和，不等同于主线程关键路径，也不能与上述嵌套函数时间相加。
没有禁用同步、修改返回值或变更持久化语义。

关键代码：

- `src/db/collection.cc`：optimize 先调用 switch_to_new_segment_for_writing，dump 后立即创建新 segment。
- `src/db/index/segment/segment.cc`：dump = flush + inverted seal。
- `src/db/index/column/inverted_column/inverted_column_indexer_write.cc`：
  generate_statistical_indexes 开始 reset 范围 CF；单文档生成一个范围槽后，
  `num_range_slot_created >= term_count` 又成立，再 reset 一次。reset 实际执行 DropColumnFamily + CreateColumnFamily。
- `src/db/common/rocksdb_context.cc`：create 顺序创建 CF；flush/compact 遍历所有 CF 后还调用默认 CF 版本。
  cf_handles_ 已包含 default，存在重复调用（性能收益需另测，不能直接当作主要原因）。

结论：原生层小集合的 RocksDB 建库、CF 元数据维护及同步是主要开销；不需要 Node.js 即可出现。
尚未将 Windows 剩余耗时全部拆到内核、文件系统或杀毒软件，不能据此归因于某个 Windows 内核缺陷。
strace 多线程汇总中的 futex 占比含后台等待，不能解释为 optimize 主线程全部在等待。

## 可采取的方向

1. 优先研究小基数时避免“生成范围索引再丢弃”，以及避免重建已知为空的范围 CF；保留 CDF 和查询语义。
2. 研究批量创建 column families、减少重复元数据写入，或延后创建新的空写入 segment。
3. 如果业务仅需文件 ID 等值筛选，可评估 `InvertIndexParams(false)`；本实验降至 Windows 约 36 ms。
   若业务需要范围查询，应另外验证查询性能，不能把本消融直接作为通用配置建议。
4. 小批量元数据更新宜合并后按阶段 optimize，不要每条 upsert 后执行；具体频率需结合写入量和查询延迟决定。
   optimize 是索引维护操作，不应当作每次写入必需的 API。持久化要求应单独检查 flush/close/WAL 契约。

首次排查仅复现和定位，未修改生产行为，也未发布 GitHub 评论。后续修复见下文。

## 本机原始证据

- `build_/r/issue743_results/`：两端三组 CSV、Linux 函数计时、汇总脚本。
- `build_/r/issue743_profile/results.csv`、`timings.log`：Windows 函数计时。
- `build_/r/issue743_profile/io_results.csv`、`io_timings.log`、`win_io.cc`：Windows Win32 计时及诊断代码。
- `build_/r/issue743_profile/generate.py`：Windows 插桩副本生成脚本；生产文件未插桩。

build_ 中的证据和诊断编译产物被 gitignore 排除，保留在当前工作区供复核。

## 后续修复（2026-09-10）

在 `a9105d35` 工作区实现了两处改动。该提交的数据库源码与上面的
`44d8bc6` 相同，修复没有改动向量算法。

- `generate_statistical_indexes()`：先检查范围 CF 是否为空；仅对残留数据或
  读取异常执行原有 reset。每槽目标文档数 <= 1 时不生成无收益的范围槽，
  保留 CDF 统计、范围查询的原始 term 扫描回退及持久化格式。
- `RocksdbContext::create()`：用 `CreateColumnFamilies` 的 descriptor 重载
  批量建 CF，保留逐 CF merge operator，OPTIONS 文件由每个 CF 写一次改为
  整批写一次。失败时释放 API 返回的部分成功 handles；default CF 单独登记。

没有关闭 FlushFileBuffers、修改 WAL 设置或延迟持久化，也没有改变新写入
segment 的生命周期。大的范围索引仍使用原有构建算法。

两端均为 Release，同一台机器修复前后对照，每种 concurrency 各 10 次，中位数 ms：

| 操作 | 修复前 c=0 | 修复后 c=0 | 修复前 c=1 | 修复后 c=1 |
| --- | ---: | ---: | ---: | ---: |
| optimize | 61.169 | 35.639 | 60.008 | 35.620 |
| create | 42.573 | 36.495 | 42.572 | 35.764 |

上表为 Windows。Linux ext4 的对应结果：

| 操作 | 修复前 c=0 | 修复后 c=0 | 修复前 c=1 | 修复后 c=1 |
| --- | ---: | ---: | ---: | ---: |
| optimize | 25.657 | 14.710 | 25.499 | 14.829 |
| create | 15.553 | 12.393 | 15.435 | 12.447 |

两端 optimize 均降低约 41–43%。Win32 诊断版确认每次 FlushFileBuffers 由 50 次降为
34 次，CreateFileW 由 64 次降为 44 次，CloseHandle 由 109 次降为 77 次。
仍有约 36 ms 的剩余维护开销，不能视为已经消除 Windows 与 Linux 的全部差异。

新增回归测试：

- `inverted_column_indexer_small_test`：空集合、1/99/999/1000/1999/2000/2001
  条文档；EQ/LT/LE/GT/GE、CDF、重开；全 NULL；模拟中断留下的范围数据。
  检查新 CF 的 ID 未因 seal 改变，残留数据存在时则被实际清理。
- `rocksdb_context_test`：默认/逐 CF merge operator 的写入和重开、default-only、
  批量创建部分成功后失败的 handles 清理和再次打开。

Windows 和 Linux 上述 13 个新增用例全部通过。Windows 另外通过现有字符串
倒排 3 个、数值倒排/快照 8 个、Collection optimize/FTS 6 个用例，共 30 个。
覆盖 Collection 的普通 optimize、重复 optimize、删除、普通 schema、索引操作和
FTS 全局 doc ID 间隙。最终 Linux 构建无新增测试告警，两端 diff 检查通过。

Linux 复用此前隔离目录的数据库对象和依赖，只重编译两处生产改动并重新静态链接。
基线可执行文件单独保留为 `issue743-build/issue743_before`；源码传输已获用户明确授权。

本轮原始结果：`build_/r/issue743_fix/windows_before.csv`、`windows_after.csv`、
`io_timings.log` 以及同目录测试日志。Linux 原始结果已取回同目录的
`issue743_fix_linux_before.csv`、`issue743_fix_linux_after.csv`。
Linux 最终编译脚本为同目录 `linux_fix_build.py`。

## 补充：Windows 文件操作成本

进一步用独立 C++ 程序排除 RocksDB 和索引算法：每组新建 200 个文件，逐个写入
4KB，分别计时 create/write/首次同步/无新写入再次同步/close/delete，取各操作中位数。
Windows 用 CreateFileW/WriteFile/FlushFileBuffers，Linux 用 open/write/fdatasync。
以下为第二轮复测（Windows 在沙箱外，结果与第一轮一致），单位微秒：

| 操作（有同步组） | Windows C: NTFS | Linux ext4 |
| --- | ---: | ---: |
| 创建文件 | 137.80 | 3.738 |
| 写入 4KB | 52.40 | 1.970 |
| 第一次文件同步 | 592.40 | 330.107 |
| 没有新写入，再同步 | 9.65 | 0.537 |
| 关闭句柄 | 40.50 | 0.652 |
| 删除文件 | 167.70 | 8.256 |

无同步组的 Windows 创建/删除也分别为 130.35/134.45 us，Linux 为 3.32/3.07 us。
因此差异不仅是刷盘：本环境的 Windows 小文件和元数据操作链路有明显固定成本，
单次首次同步则约相差 1.8 倍。此微基准和 RocksDB 的文件大小、打开方式、线程模型不同，
不能把这些中位数直接乘以前面的调用次数，当成 optimize 的精确分解。

还找到源代码层面的平台差异：RocksDB `WinWritableImpl::CloseImpl()` 会额外
FlushFileBuffers；`PosixWritableFile::Close()` 不额外执行 fsync/fdatasync。
但无新写入重复同步的微基准成本较小，不能把这条差异单独当作主要原因。

Windows C: 是 Alibaba Cloud NVMe 云盘上的 NTFS；Linux 为 Alibaba Cloud Elastic
Block Storage 上的 ext4。设备产品类型相同不表示配置、性能或负载完全相同。
Defender 实时防护开启，但已有 `C:\w` 路径排除，涵盖本次测试目录；因此没有证据把
本次差异主要归因于 Defender 内容扫描。检查中没有关闭防护或修改排除配置。

本次定位止于：Windows 文件操作/元数据固定成本更高，文件同步也更慢，RocksDB
的高频 CF 维护放大了差异。不区分 NTFS、其他过滤驱动和云存储各自的贡献，
也不能把本环境数值推广为所有 Windows/Linux 的固定倍率。
微基准源文件：`build_/r/issue743_profile/file_cost.cc`。

## Dev Drive 对照

在 C:/D: 之间交替运行三轮相同二进制，每轮每种 concurrency 各 5 次。
D: 是 trusted Dev Drive、ReFS、File Backed Virtual Disk，Defender performance mode=1。
`fltmc instances` 显示 D: 仅挂 WdFilter；C: 挂 bindflt、FsDepends、AliSecGuard、
WdFilter、bfs、luafv、Wof。只读取配置，没有卸载/停用过滤驱动。

合并 15 次/并发配置后的 optimize 中位数：C: c=0/1 为 62.617/62.692 ms；
D: 为 62.508/60.095 ms，整体未出现大幅改善，全部写入/fetch 检查通过。

每轮 200 个小文件的中位数范围（us）：

| 操作 | C: NTFS | D: ReFS Dev Drive |
| --- | ---: | ---: |
| 无同步组创建 | 132–133 | 25–31 |
| 无同步组删除 | 130–144 | 31–38 |
| 首次同步 | 551–610 | 619–713 |
| 无新写入再次同步 | 9.5–11.3 | 389–635 |

D: 原生 optimize 诊断中仍有 50 次 FlushFileBuffers，累计 28–34 ms，
64 次 CreateFileW 累计 2.2–2.6 ms。创建更快，但同步更贵，抵消了收益。
特别是重复同步，Dev Drive 此配置下并不便宜。

这支持 C: 的额外过滤链或 NTFS 路径可能增加元数据成本，但不能单独证明
AliSecGuard 或其他某个 minifilter 是原因：此对照同时改变了文件系统、过滤链、
Defender 策略和 VHD 存储层。D: 去掉其他过滤驱动后 optimize 仍慢，也不支持
把全部延迟归于 C: 特有 minifilter。后续若要单独归因，应在专用测试环境同卷同文件系统
控制过滤驱动变量，不应直接关闭生产机器安全防护。

原始 CSV 和 Win32 计时：`build_/r/issue743_results/devdrive_20260910163322/`。

### 用户添加 D:\ Defender 排除后的复测

确认 ExclusionPath 包含 `D:\`，重复三轮交替 C/D 实验，每种 concurrency 共 15 次。
没有由测试程序修改防护配置。

| 指标 | D: 排除前 | D: 排除后 | C: 本轮对照 |
| --- | ---: | ---: | ---: |
| optimize c=0 中位数 ms | 62.508 | 58.849 | 62.484 |
| optimize c=1 中位数 ms | 60.095 | 55.986 | 63.013 |
| 无同步组创建文件 us | 25–31 | 9.2–10.6 | 123–138 |
| 无同步组删除文件 us | 31–38 | 15.5–16.4 | 130–140 |
| 无新写入再次同步 us | 389–635 | 367–570 | 10–11 |
| 原生 optimize 50 次 FlushFileBuffers 累计 ms | 28–34 | 27–31 | 19–22 |

排除后创建/删除/关闭的成本下降，optimize 中位数约改善 6–7%；C: 对照基本稳定。
因此 Defender 确实很可能贡献了 D: 的部分小文件开销，但昂贵的重复同步仍存在，
它并未被排除配置消除。D: 本轮 optimize 个别样本升至 84–99 ms，存在波动，
不能把前后中位数差当作严格受控的收益。此结果支持继续区分防护扫描成本与
ReFS/虚拟磁盘同步路径成本，而不是把后者全部归因于 Defender。

原始证据：`build_/r/issue743_results/devdrive_excluded_20260910164158/`。
