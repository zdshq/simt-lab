# 开发记录

## 阶段 0：建立工程边界（2026-10-29）

- 根据本地 curryGPU 文章，采用先功能、后硬件的路线，本次实现功能级 ISS。
- 在 `simt-lab/` 建立独立仓库，保留上级已有资料。
- 默认目标为两个 SM、32 lane/warp；执行器使用 C++20，编码生成和参考测试使用 Python 标准库。
- 当前环境具备 GCC 11.4、CMake 3.22、Ninja 和 Python 3.13，无需先安装依赖。
- 第一阶段先验证编码与基础执行，后续按计划扩大机器状态和指令语义。

## 阶段 1：编码与可运行执行器（2026-10-29）

- 实现 25 条基础指令，统一 JSON 描述生成 C++ 解码表与 Python 编码器；128 位小端编码，保留位严格校验。
- 执行器支持可配置 SM 数、CTA 派发、多 warp 和部分有效 warp；默认两个 SM，每 SM 一个驻留 CTA。
- 实现整数、谓词、基础浮点与融合乘加；结构化 JSON 结果包含实际 SM 派发和架构状态。
- 收到用户 `$coding-standards` 要求后，先将执行、调度、转储、CLI、生成器拆成短函数，补齐相邻中英文注释；项目 AGENTS.md 固化约定。
- 验证：`cmake --build build` 成功；`ctest --test-dir build -V` 的 12 项测试通过，包括 1,000 次编码往返、80 个逐 lane 整数差分程序、融合舍入反例和错误诊断。
- 示例：`python3 examples/identifiers.py` 后运行 `./build/simt-run build/identifiers.bin --sms 2 --blocks 5 --threads 37`，5 个 CTA 完成，SM 分布为 0、1、0、1、0。
- 此阶段仍无跳转重聚、内存或 MMA，下一阶段验证分歧与线程集合的正确性。
