# 来源与移植范围

从用户的 CUTLASS 项目 `tools/gemm_tile_web` 提取：

- `metrics.py`：保留 NCU 指标词典、单位换算、长/宽 CSV 与多 launch 解析，去除 GEMM/Attention 的原生结果及特定函数解析。
- `diagnostics.py`：保留 L1/L2/spill 证据式规则，移除原项目历史数据库依赖，改成按具体函数提供编译证据。
- `catalog.py`、`learning.py`：复用中文指标定义、机制与课程；通用界面过滤算子专用输入字段，并标注教学例子与实际值的区别。

新增 `perf.py`、`static/dashboard.html` 提供命令适配、历史报告导入、记录、对比与可移植网页。无需原项目 CUDA 头文件、数据库、Nsight 报告或机器级 sudo 安装。

`nsys_analysis.py` 提供独立的 Nsight Systems SQLite 解析和活动区间并集；同一工具包加入 NSYS 采集/导入、可缩放时间线与原生 Nsight GUI 打开命令。NCU 和 NSYS 保留各自数据口径与原始文件。

仓库许可证随 `LICENSE.txt` 保留。Python 资源只用标准库。NCU/NVIDIA 驱动是可选的外部采集工具，没有重新分发。
