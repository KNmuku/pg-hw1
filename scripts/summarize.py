#!/usr/bin/env python3
"""Summarize actual benchmark JSON; never substitute missing PMU values."""
import csv
import json
import sys
from pathlib import Path

directory = Path(sys.argv[1])
order = ['random32', 'random64', 'random128', 'random512', 'prefix64',
         'prefix512', 'duplicate64', 'miss64', 'hit64', 'short1000', 'many_lists']
rows = []
for name in order:
    path = directory / (name + '.json')
    if not path.exists():
        continue
    data = json.loads(path.read_text(encoding='utf-8'))
    assert data['verification'] == 'passed'
    linear, indexed = data['results']
    assert linear['hits'] == indexed['hits'] == data['expected_hits']
    assert linear['checksum'] == indexed['checksum']
    assert len(linear['timing_samples_ns_per_query']) == data['config']['repeats']
    for result in data['results']:
        rate = result['l1d_hit_rate_percent']
        assert rate is None or 0 <= rate <= 100
    rows.append((name, data))

with (directory / 'summary.csv').open('w', encoding='utf-8', newline='') as stream:
    writer = csv.writer(stream)
    writer.writerow(['case', 'nodes_per_list', 'lists', 'size', 'hit_rate_percent',
                     'linear_ns_per_query', 'indexed_ns_per_query', 'speedup',
                     'prepare_ms', 'bytes_before_index', 'bytes_after_index'])
    for name, data in rows:
        c = data['config']
        a, b = data['results']
        writer.writerow([name, c['nodes_per_list'], c['lists'], c['size'],
                         c['hit_rate_percent'], a['ns_per_query_median'],
                         b['ns_per_query_median'], data['speedup'], data['prepare_ms'],
                         data['list_bytes_before_index'], data['list_bytes_after_index']])

lines = ['# 实测结果', '',
         '每项包含固定种子、逐查询正确性校验、预热和 5 次计时。表中时间为每次查询的中位耗时。', '',
         '| 场景 | 遍历 ns/次 | 索引 ns/次 | 加速比 |',
         '|---|---:|---:|---:|']
for name, data in rows:
    a, b = data['results']
    lines.append(f"| {name} | {a['ns_per_query_median']:.2f} | {b['ns_per_query_median']:.2f} | {data['speedup']:.2f}× |")
lines += ['', '完整计数值、事件可用性、计时样本和测量口径见对应 JSON。',
          '指令数、周期数及访存计数为每批查询计数在多次测量间的平均值；L1D 为通用 READ 事件映射的估算命中率。',
          '访存使用 AMD Zen5 LS dispatch 时包含推测执行，不等同于退休访存指令或 DRAM 事务。',
          'WSL2 的 PMU 数据受虚拟化影响；结果只代表 environment.txt 中记录的本次环境。', '']
(directory / 'summary.md').write_text('\n'.join(lines), encoding='utf-8')
print(f'Validated {len(rows)} benchmark JSON reports.')
