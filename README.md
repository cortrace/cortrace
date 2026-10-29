<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-wordmark-dark.svg">
    <img alt="cortrace" src="docs/assets/logo-wordmark-light.svg" height="72">
  </picture>
</p>

# Cortrace

**简体中文** | [English](README_en.md)

**Cortrace** 是 Cortex-M 硬件 trace 的 PC 端工具链。目标芯片的 ETM、DWT、ITM 从 4 位并行 trace 口输出，
[cortrace-fpga](https://github.com/cortrace/cortrace-fpga) 采集器把它经千兆网送到 PC（TRACECLK 75 MHz 时约 75 MB/s）；cortrace 负责去帧，
用 ARM/Linaro 的参考解码器 [OpenCSD](https://github.com/Linaro/OpenCSD) 解码，然后给出两种结果：

- **时间线**：函数级调用栈、异常和中断各自的轨道、RTOS 线程泳道，写成 [Perfetto](https://ui.perfetto.dev) trace，
  在浏览器里打开。线程名由 OS 插件（内置 NuttX、FreeRTOS）从 ELF 和目标内存读出。
- **代码覆盖率**：不改固件、不插桩，按指令、源码行、分支结局和函数统计，输出文本（Ozone 风格）、JSON、lcov、
  Cobertura。`cortrace cover stream` 可以连续抓几个小时，原始 trace 不落盘。

trace 链路没有校验，所以 cortrace 不猜：丢掉的数据在结果里如实标出，不会拿过期的 PC 编造程序流；覆盖率只统计
通过静态校验的干净 trace 段，报告里写明丢了多少。

目前在 STM32H743（Cortex-M7，ETMv4）上验证过裸机、NuttX 和 FreeRTOS 固件，以 Ubuntu 22.04 的 `.deb` 发布。
要把 NuttX 的软件 note 合到同一条时间轴上，用 pynuttx 的 `nxtrace merge`。

> 设计与路线图见 [`docs/00-architecture.md`](docs/00-architecture.md)，覆盖率的设计和实测见
> [`docs/07-realtime-coverage.md`](docs/07-realtime-coverage.md)。

## 安装（Ubuntu 22.04 amd64）

从 [GitHub Releases](https://github.com/cortrace/cortrace/releases) 下载 `cortrace_<版本>_amd64.deb`：

```sh
sudo apt install ./cortrace_*_amd64.deb      # 自动装上 python3 和 binutils-arm-none-eabi
cortrace --help
```

装好后无需克隆仓库，也不需要任何特权（`cortrace-grab` 在 Linux ≥ 5.7 上免 root、免 setcap）。

| 命令 | 作用 |
|------|------|
| `cortrace capture` | 从 FPGA 抓一段并口 trace → 解码成 Perfetto（`--notes` 另外写出 ITM 上的 note 字节流和硬件线程切换日志，给 `nxtrace merge` 用） |
| `cortrace serve` | 在 ui.perfetto.dev 的 *Record new trace → Linux → WebSocket 127.0.0.1:8037* 里点 **Start** 即抓取，结果回到同一页面 |
| `cortrace cover` | 代码覆盖率：`run` 抓几秒离线解码，`stream` 连续抓几小时、原始数据不落盘；`report`/`merge`/`diff` 换格式、合并、对比 |
| `cortrace decode` | 直接调用 `cortrace-decode`（ETM/DWT/ITM → Perfetto），参数原样透传 |
| `cortrace tcbmap` | 读活线程名映射 |
| `cortrace arm` | 通过常驻 OpenOCD 布防 ETM/ITM/DWT（芯片见 `platforms/`） |
| `cortrace os` | OS 插件（NuttX、FreeRTOS 内置）：列出、查监视变量、解析线程名 |
| `cortrace fpga ctrl\|net\|health` | FPGA 采集器的 CSR 控制、链路发现、健康读出 |
| `cortrace open` | 在 ui.perfetto.dev 打开一个 trace 文件 |

**输出目录必须显式给出**：`--out-dir DIR` 或 `export CORTRACE_OUT_DIR=DIR`。抓取约 75 MB/s，
解码产物约为原始数据的 5 倍（1 秒 ≈ 450 MB），所以 cortrace 不会替你选位置；开始前会估算所需
空间并在不够时直接报错，超过 5 秒的抓取需要 `--allow-long`。cortrace 不会自动删除任何文件。

```sh
export CORTRACE_OUT_DIR=~/traces
cortrace capture --iface enx0123 --elf fw.elf --secs 1 --width 4 --open
cortrace serve   --iface enx0123 --elf fw.elf          # 然后在网页里点 Start
```

> 抓取前先布防目标的 ETM/DWT/ITM：用 `cortrace arm`（经常驻 OpenOCD）或自己的调试器，并保持连接。
> 和 NuttX 的 note 合并由 pynuttx 的 `nxtrace merge` 完成，cortrace 不依赖 pynuttx。

**完整的使用步骤（抓取解码、带 note 的抓取、网页点击、排错）见 [`docs/04-user-guide.md`](docs/04-user-guide.md)。**

## 为什么

其他项目使用的精简 ETMv4 解码器在采集质量边际时会出错：遇到损坏字节就带着过时的 PC
继续行走，捏造多达约 4.6 倍的真实指令数和数百次假函数重入。OpenCSD 对同一条流处理正确，
并把无法解码的区段如实标记，而不是编造程序流。Cortrace 把最难的 ETM 解码外包给 OpenCSD，
只拥有其上确定性的层——调用栈重建、时间基对齐、Perfetto 导出。

原型已产出与 ELF **逐条调用边完全一致（0 mismatch）** 的调用图，且嵌套配平。
当前 `cortrace-decode` CLI 已跑通完整离线链路（OpenCSD 真链库 → 调用栈机），在 CoreMark
slice 上复现该结果：**11/11 调用边对 ELF、0 mismatch、begin/end 配平、14 个 SysTick 渲染**。

## 从源码构建

需要 CMake ≥ 3.16、C++17 编译器，以及（可选）`libopencsd-dev` 用于解码器适配层。
核心库 + 测试在没有 OpenCSD 时也能构建；装了 OpenCSD 时额外产出 `cortrace-decode` CLI。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

自己打 deb（在 ubuntu:22.04 容器里构建，运行时依赖才与 22.04 一致；版本取自最新的 `vX.Y.Z` tag）：

```sh
scripts/build-deb.sh dist                       # 产出 dist/cortrace_<版本>_amd64.deb
scripts/smoke-deb.sh dist/cortrace_*.deb        # 干净容器 + 普通用户的冒烟测试
```

推送 `v*` tag 时，CI 会构建 deb、跑冒烟测试，并把它挂到对应的 GitHub Release。

### 版本号

唯一来源是仓库根目录的 `VERSION` 文件（第一版是 `1.0.0`），格式为 [PEP 440](https://peps.python.org/pep-0440/) 的子集：
`主.次.补丁`，可加后缀 `aN`/`bN`/`rcN`（预发布）、`.postN`、`.devN`，例如
`1.0.0`、`1.0.0a1`、`1.0.0rc1`、`1.0.0.post1`、`1.0.0.dev3`。

| 构建 | `cortrace version` / `cortrace-decode --version` |
|------|--------------------------------------------------|
| HEAD 正好是 `v<VERSION>` tag（发布） | `1.0.0` |
| 其他提交（开发构建） | `1.0.0+git<提交数>.<哈希>` |
| 未构建的源码目录直接运行 | `1.0.0+dev` |

deb 里用 Debian 写法，保证 `apt` 的排序与 PEP 440 一致：`1.0.0a1` → `1.0.0~a1`（排在 `1.0.0` 之前），
`1.0.0.post1` → `1.0.0+post1`。发布步骤：改 `VERSION` → 提交 → `git tag v<VERSION>` → 推送；
CI 会校验 tag 与 `VERSION` 一致，`a`/`b`/`rc`/`dev` 的 tag 在 GitHub Release 上标为预发布。

## 离线解码（cortrace-decode）

在已 deframe 的 ETMv4 字节流上跑完整链路，报告质量指标（配平、调用边）。
`mem.bin` 是 ELF 的扁平镜像，`mem_base` 是其最低 LMA（STM32H743 一般是 `08000000`）。

```sh
arm-none-eabi-objcopy -O binary fw.elf mem.bin      # 扁平镜像
arm-none-eabi-nm fw.elf > syms.nm                   # 符号表
build/cortrace-decode capture.etm.bin mem.bin 08000000 syms.nm --edges edges.tsv
```

判据：`begins == ends`（配平）、`edges.tsv` 每条边都对应 ELF 里真实的 `bl`（0 mismatch）。

## 覆盖率

```sh
cmake -S . -B build -DENABLE_COVERAGE=ON
cmake --build build --target coverage   # 跑测试 + gcovr，低于门禁则失败
```

门禁阈值用 `-DCORTRACE_MIN_COVERAGE=<百分比>`（默认 95）。HTML 报告在
`build/coverage-html/`。

## 代码格式化

基于 WebKit 的 clang-format（见 `.clang-format`）。

```sh
scripts/format.sh          # C/C++ 原地格式化（clang-format）
scripts/format.sh --check  # CI 模式：有差异则失败
```

Python 包（`python/cortrace`）用 black 格式化 + pylint 检查（见 `pyproject.toml`、
`.pylintrc`）：

```sh
scripts/format-py.sh          # black 原地格式化
scripts/format-py.sh --check  # CI 模式：black --check + pylint
python -m pytest python/tests --cov --cov-fail-under=90   # 测试 + 覆盖率门禁（90%）
```

## Git 钩子

在每次提交时强制格式检查：

```sh
scripts/install-hooks.sh   # 设置 core.hooksPath = .githooks
```

`pre-commit` 钩子会在任何已暂存的 C/C++ 源码不符合 `.clang-format`、或 Python 源码不符合
black/pylint 时**阻止提交**。`commit-msg` 钩子要求主题为 `type(scope): 描述`，只有 `docs`
可以省略 scope。

## 目录结构

```
include/cortrace/   公共头文件（element / symbols / callstack …）
src/                核心实现（不依赖 OpenCSD）
tools/              cortrace-decode（C++）、cortrace-grab（C，FPGA UDP 采集）
python/cortrace/    cortrace 命令行与 Python 包（capture / serve / tcbmap / fpga …）
python/tests/       Python 单元测试
tests/              C++ 零依赖单元测试 + 框架
cmake/              CodeCoverage.cmake（gcovr + 门禁）、Packaging.cmake（install + deb）
packaging/debian/   deb 的 postinst / prerm
scripts/            format*.sh、install-hooks.sh、build-deb.sh、smoke-deb.sh
.githooks/          pre-commit（格式/lint 门禁）、commit-msg（提交主题）
.github/workflows/  ci.yml（格式 + 构建 + 测试 + 覆盖率 + Python 门禁 + deb）
docs/               设计文档（架构、Perfetto 直连/融合/Record 桥……）
```

## 相关仓库

- [**cortrace-fpga**](https://github.com/cortrace/cortrace-fpga) — Artix-7 并口 ETM 采集器的 RTL 与板级调试小工具（PC 侧正式工具都在本仓库）
- [**stm32h743-etm-trace-firmware**](https://github.com/cortrace/stm32h743-etm-trace-firmware) — 确定性 selftrace 目标板固件

## 许可证

MIT © 2026 VIFEX（见 [`LICENSE`](LICENSE)）。包内静态链接的 OpenCSD 为 BSD-3-Clause（见
`/usr/share/doc/cortrace/LICENSE.opencsd`）。
