# 主机接口、CP 与任务图扩展

本次用户要求补齐主机提交链路，并询问 CUDA Graph。交付目标为模拟器自己的持久设备接口及 CUDA Graph 风格的功能语义，不宣称 NVIDIA CUDA ABI、PTX、驱动或 PCIe 兼容。

| 阶段 | 内容 | 验收 | 状态 |
|---|---|---|---|
| 7 | 持久设备内存、可推进 kernel、有界提交队列、doorbell、CP、DMA 与完成状态 | 三项原生协议测试；命令取走不等于完成；失败请求不部分提交 | 已完成，04f78cb |
| 8 | Python 主机接口、stream 顺序、跨 stream event、同步与跟踪 | 九项主机测试；H2D→kernel→D2H；多 stream 依赖、超时、资源保留 | 已完成，dc95372 |
| 9 | 显式 DAG、实例化、一次提交启动整图、重复启动、节点参数更新、stream capture | 11 项图测试；循环拒绝、fork/join、独立分支、capture 不执行、失败传播、更新与重放 | 已完成，1b1833a |
| 10 | 可运行示例、文档、全量回归及 sanitizer | Debug/Release/ASan+UBSan 均通过九套件、75 项测试；65 元素向量示例独立比较；阶段记录见 DEVELOPMENT.md | 已完成 |

## 实现约定

- C++ `simt-device` 进程拥有持久内存与 CP；Python 通过结构化请求调用，不通过重启单次 CLI 冒充持久设备。
- 有界环形提交队列采用单调 write/read/doorbell 指针；doorbell 只发布已写命令，完成通过独立 job 状态通知。
- 一次推进只服务有限工作；一个 kernel 引擎使用设备的两个 SM，一个 copy 引擎独立推进，可重叠执行。暂不让多个 kernel 分割 SM 同时执行。
- 通过显式 progress/synchronize 驱动确定性的功能事件循环；不预测硬件周期，也不承诺无主机调用时自动前进。
- kernel 继续由 C++ ISA 执行器计算；启动参数置于该 kernel 的 constant memory。
- 图实例保留经过检查的节点与依赖。一次 graph launch 进入提交队列，CP 展开内部节点；完整图完成才通知主机。
- 图包含 H2D、D2H、D2D、memset、kernel、empty 节点。支持显式建图与基于 stream/event 的捕获；不承诺 CUDA Graph 全部节点类型与 API 行为。
- 当前沿用短函数、入口只调用流程函数及相邻中英文注释规范。
