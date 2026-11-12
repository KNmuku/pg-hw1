# HW1 无 HASH 双向循环链表搜索与性能分析

本项目完成 `link1.md` 的 C 语言双向循环链表程序，并用 `perf_event_open()` 对照普通遍历和有序索引搜索。程序面向 Linux，GCC 默认使用 `-O2`，不使用 `-O3`。实测原始数据放在 `reports/20261112/`。

## 编译和运行

需要 Linux、GCC、GNU make 和 Linux 用户态头文件。汇总脚本额外使用 Python 3 标准库。Ubuntu 或已有 WSL2 终端内执行：

```sh
cd hw1
make
make test
make sanitize
./build/bench --nodes 10000 --queries 10000 --size 64 --repeats 5 --require-perf
make benchmark
```

本机 Windows 的对应目录可在 WSL 中以 `/mnt/h/rw/课件/开源/PG/作业一/hw1` 访问。文件夹中已包含源码和结果，上传 GitHub 时保留这些文件即可；`build/` 为可重新生成的构建产物，已加入 `.gitignore`。本项目没有初始化 Git、创建远程仓库或上传。

```sh
# 可选：针对运行机器的指令集编译，仍为 -O2
make native
./build/native/bench --size 512 --pattern prefix

# 最小接口示例
gcc -O2 -std=c11 -Iinclude examples/demo.c src/link.c -o build/demo
./build/demo

# 保存一组可复核的原始结果；REQUIRE_PERF=1 要求所有指标有效
REQUIRE_PERF=1 sh scripts/run_bench.sh ./build/bench reports/my-run
```

## 数据结构和接口

保留原题 `LinkData` 的 `prev`、`next`、`type`、`dataSize` 和 `data`。在本次 x86-64 ABI 下 `sizeof(LinkData)=32`。`dataSize` 的有效范围为 32～512 字节，按二进制缓冲区处理，包含 `\0` 的内容也可搜索。这里 32～512 字节指数据区长度，节点管理字段另计。

```c
LinkData *InitLink(int size, char *mem);
void AppendNode(LinkData *firstNode, int size, char *mem);
LinkData *FindNode(LinkData *firstNode, int size, char *mem);
```

- `InitLink` 复制输入数据，返回包含第一条数据的头节点；初始 `prev` 和 `next` 均指向自己。
- `AppendNode` 按插入顺序在尾部追加，维护 `tail->next` 和 `head->prev`。兼容题目的 `void` 签名。实际应用建议调用返回新节点指针的 `AppendNodeChecked`；失败返回 NULL 并设置 `errno`，原链表不变。
- `FindNode` 返回长度和全部字节都相等的节点；重复数据返回从头遍历遇到的第一个节点。未命中返回 NULL。
- `FindNodeLinear` 从头开始逐节点遍历，是与原题遍历描述一致的对照实现。`FindNode` 使用题目允许的辅助索引优化，保持相同返回结果，不承诺逐节点访问。
- `PrepareLink` 可提前构建索引；`FindNode` 也可按需构建。追加后旧索引失效，下次搜索重建；构建内存不足时自动回退到正确的顺序遍历。
- `DestroyLink` 释放链表和索引；`LinkCount`、`LinkMemoryBytes` 提供节点数和申请内存字节数。

所有 `firstNode` 参数都必须是 `InitLink` 返回的原始头指针。数据由链表持有，调用者可以立即释放输入缓冲区。若直接修改节点内容，必须先确保长度及数据指针不变，并调用 `InvalidateLinkIndex(head)`；否则索引失效。程序不提供删除和并发写入接口；同一条链表并发访问应由调用者同步。

## 搜索优化和正确性

长度小于 32 个节点的链表不建立索引，避免对数百万条短链表预分配大表。较长链表建立连续的有序数组，每项存放数据长度、原始前 8 字节、节点指针和插入序号。排序键为：

1. 数据长度；
2. 原始前 8 字节的字典序；
3. 其余字节的 `memcmp` 字典序；
4. 插入序号。

搜索使用 `lower_bound` 二分，比较完整键且忽略最后的插入序号，取最左侧相等项。因此不会把仅有前缀相同的数据误判为相等；最左侧的相同键具有最小插入序号，与从头遍历的第一个结果一致。

这不是 HASH：没有哈希函数、哈希桶、散列取模或布隆过滤器。前 8 字节只是原始数据的顺序副本；小端机器上的字节交换仅用于保持字典序。测试数据生成使用的伪随机数不参与任何搜索加速。

设节点数为 N、数据长度为 L。普通搜索最坏 `O(NL)`；索引准备 `O(N log N · L)`，稳定后的搜索最坏 `O(L log N)`，额外索引空间 `O(N)`。相同长前缀不会破坏正确性或二分的对数比较次数，但会增加每次比较读取的字节数。本文的优化适合批量构建后反复查询；若每追加一次就查询，重复建索引的代价可能超过收益。

节点及数据只需一次分配，减少单独数据块分配。元数据只放在头节点中，普通节点不承担每链表元数据。索引项在实测 ABI 下为 32 字节，不是预先为每条链表分配一个固定大容量数组。`memcmp` 使用系统 C 库的实现，程序不手工越界读取 32～512 字节缓冲区。

## 四项性能指标的口径

| 指标 | 事件或计算方法 | 说明 |
|---|---|---|
| 指令数 | `PERF_COUNT_HW_INSTRUCTIONS` | 用户态退休指令数 |
| 周期数 | `PERF_COUNT_HW_CPU_CYCLES` | 用户态 CPU 周期，不等同墙钟时间 |
| 访存次数 | 本机自动选择 AMD Zen5 `ls_dispatch`：raw `0x129 + 0x629` | 加载操作 + 存储和读改写操作的派发次数；包含推测执行，不是退休访存指令数或 DRAM 事务数 |
| L1 Cache 命中率 | `100 × (1 - L1D.READ.MISS / L1D.READ.ACCESS)` | 按 Linux 通用事件映射估算 L1 数据缓存命中率，不包含 L1 指令缓存 |

本机 CPUID 为 `AuthenticAMD`、family `0x1a`、model `0x44`，对应 Ryzen 9 9950X3D。只有这个已核对的型号自动选择 Zen5 raw 事件：EventCode `0x29` 的 UnitMask `0x01` 为 load、`0x02` 为 store、`0x04` 为 load-op-store，后两者合为 `0x06`，总和等于 `ls_dispatch.all` 的 `0x07`。不要把这些 raw 编码原样用于其他厂商 CPU。

其他 CPU 默认尝试 L1D 通用 READ/WRITE ACCESS 作为访存代理量；若写事件不支持则该项为 N/A。可以根据对应 CPU 官方事件表用 `--loads-raw` 和 `--stores-raw` 提供两个可相加的互斥事件。两个参数必须同时给出，名称本身不保证它们是退休事件。

L1D 的通用 READ 名称不保证所有 CPU 都严格只统计读操作。内核可能映射到更宽泛的数据缓存访问或补充请求，故输出为事件口径的估算命中率。不同 CPU、内核或虚拟化环境的这个百分比不能直接等同或横向比较。未使用通常指末级缓存的 `PERF_COUNT_HW_CACHE_REFERENCES/MISSES` 冒充 L1 指标。

计数器按两事件一组测量：指令和周期一组、L1D 访问和未命中一组、访存两个分量一组。组内同时启停，三组分别重放完全相同的查询批次。设置 `pid=0,cpu=-1` 只测当前线程，排除 kernel 和 hypervisor，使用 close-on-exec。通过事件 ID 对应返回值，检查 `read/ioctl`，并按 `raw × time_enabled / time_running` 缩放。`time_running=0` 或不可用时输出 N/A/null，不填零；JSON 保存组的可用性、错误码和实际运行比例。

## 实验方法和结果

1. 默认固定种子 `20261112`，建立数据和固定查询集合；命中率是精确的整数百分比（非整除时向下取整）。
2. 同一条链表、同一批查询分别交给普通遍历和有序索引。逐查询与已知目标指针比对，验证命中、未命中及重复数据的首节点语义。
3. 索引准备、随机数据生成、正确性检查和输出均在测量区间之外。检查同时完成预热；不宣称为冷缓存测量。
4. 固定到当前允许的首个逻辑 CPU，可用 `--cpu` 指定。计时用 `CLOCK_MONOTONIC_RAW`，每轮交替两算法先后顺序。
5. 每个场景重复 5 次，报告每查询耗时中位数和最小/最大值。硬件指标为每批计数在 5 次测量间的平均值，按查询数相除可得每查询指标。
6. 测量范围包括搜索函数及双方一致的查询循环、函数调用和结果累加开销。构建代价和内存增量另外记录。结果通过校验和参与外部可观察运算，避免优化器删除搜索。

实际结果见 [实测汇总](reports/20261112/summary.md) 和同目录 JSON、CSV、环境信息。测试覆盖 32/64/128/512 字节、长公共前缀、重复键、全命中、全未命中、1000 个节点和 20000 条单节点链表。WSL2 会引入调度及虚拟化影响，测量代表这次机器环境，不能作为所有机器上的固定加速倍数。

## 计数器不可用时

退出码 0 表示搜索和普通计时成功，默认允许部分 PMU 事件缺失；退出码 1 表示输入、分配、校验等错误；`--require-perf` 下退出码 2 表示有必需硬件指标不可用或比率无效。

权限不足通常显示 EACCES/EPERM；事件不支持可能显示 ENOENT/EINVAL/EOPNOTSUPP；未调度组可能返回零运行时间。先查看 `cat /proc/sys/kernel/perf_event_paranoid`、`lscpu` 和 `/sys/bus/event_source/devices`，在允许访问 PMU 的 Linux/虚拟机环境重试。程序不会自动修改系统设置。事件能打开也不代表语义正确，raw 事件仍须根据实际 CPU 资料核对。

## 文件组织

```text
hw1/
  include/link.h           题目接口及辅助接口
  include/perf_metrics.h   硬件事件组接口
  src/link.c               循环链表与非 HASH 索引
  src/perf_metrics.c       perf_event_open 与组读取
  src/bench.c              数据生成 校验 对照计时和统计
  tests/                  功能 随机对照 故障注入 CLI 测试
  examples/demo.c          简单使用示例
  scripts/                可复现实验及结果汇总
  reports/20261112/        已运行的测试和原始实测结果
  Makefile
  README.md
```

## 参考资料

- [PostgreSQL 15 源码安装和调试选项](https://www.postgresql.org/docs/15/install-procedure.html)
- [GCC 优化选项](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html)
- [GDB 程序参数](https://www.sourceware.org/gdb/current/onlinedocs/gdb.html/Arguments.html)
- [Linux perf_event_open 手册](https://man7.org/linux/man-pages/man2/perf_event_open.2.html)
- [Linux AMD Zen5 load-store 事件定义](https://github.com/torvalds/linux/blob/v6.18/tools/perf/pmu-events/arch/x86/amdzen5/load-store.json)
- [Linux AMD Zen5 指标定义](https://github.com/torvalds/linux/blob/v6.18/tools/perf/pmu-events/arch/x86/amdzen5/recommended.json)
- [Linux AMD 通用缓存事件映射](https://github.com/torvalds/linux/blob/v6.18/arch/x86/events/amd/core.c)
