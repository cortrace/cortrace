<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo-wordmark-dark.svg">
    <img alt="cortrace" src="docs/assets/logo-wordmark-light.svg" height="72">
  </picture>
</p>

# Cortrace

[简体中文](README.md) | **English**

**Cortrace** is the PC side of hardware trace for Cortex-M. The target's ETM, DWT and ITM leave the chip on
its 4-bit parallel trace port; the [cortrace-fpga](https://github.com/cortrace/cortrace-fpga) capture box sends
them to the PC over gigabit Ethernet (75 MB/s at a 75 MHz TRACECLK). Cortrace deframes the stream, decodes it with the ARM/Linaro
reference decoder [OpenCSD](https://github.com/Linaro/OpenCSD), and gives you two things:

- **A timeline**: function call stacks, one track per exception and interrupt, and RTOS thread lanes, written as
  a [Perfetto](https://ui.perfetto.dev) trace that opens in the browser. OS plugins (NuttX and FreeRTOS built in)
  name the threads from the ELF and the target's memory.
- **Code coverage** with no firmware changes and no instrumentation: instructions, source lines, branch outcomes
  and functions, reported as text (laid out like Ozone), JSON, lcov or Cobertura. `cortrace cover stream` runs for
  hours without writing the raw trace to disk.
- **Cycle-accurate profiling** from the ETM cycle counts: cycles and CPI per function, exception entry and run
  time, how late SysTick was, and cache on/off comparisons (`cortrace profile`).

The trace link has no checksum, so cortrace does not guess: lost data is marked as lost in the results instead of
being bridged with a stale PC, and coverage counts only clean trace segments that pass static checks, with the
losses stated in the report.

It is verified on the STM32H743 (Cortex-M7, ETMv4) with bare-metal, NuttX and FreeRTOS firmware, and ships as a
`.deb` for Ubuntu 22.04. To put NuttX's software notes on the same time axis, use `nxtrace merge` from pynuttx.

> Design and roadmap: [`docs/00-architecture.md`](docs/00-architecture.md). Coverage design and measurements:
> [`docs/07-realtime-coverage.md`](docs/07-realtime-coverage.md). Cycle profiling: [`docs/08-cycle-profile.md`](docs/08-cycle-profile.md), measurements: [`docs/09-profile-measurements.md`](docs/09-profile-measurements.md), cross-validating cachecliff: [`docs/10-cachecliff-validation.md`](docs/10-cachecliff-validation.md) (all in Chinese).

## Install (Ubuntu 22.04 amd64)

Download `cortrace_<version>_amd64.deb` from the
[GitHub Releases](https://github.com/cortrace/cortrace/releases):

```sh
sudo apt install ./cortrace_*_amd64.deb      # pulls in python3 and binutils-arm-none-eabi
cortrace --help
```

No clone is needed and no privileges either (`cortrace-grab` runs without root or setcap on
Linux >= 5.7).

| Command | What it does |
|---------|--------------|
| `cortrace capture` | grab a parallel trace from the FPGA and decode it to Perfetto (`--notes` also writes the bytes on an ITM stimulus port and the hardware thread-switch log, the inputs of `nxtrace merge`) |
| `cortrace serve` | in ui.perfetto.dev choose *Record new trace -> Linux -> WebSocket 127.0.0.1:8037* and press **Start**: the capture runs and the result comes back in the same page |
| `cortrace cover` | code coverage: `run` captures a few seconds and decodes offline, `stream` captures for hours without storing the raw trace; `report`/`merge`/`diff` convert, add up and compare |
| `cortrace profile` | cycle-accurate profile: `run` captures and processes, `report` prints, `diff` compares two experiments (caches on/off, before/after a change) |
| `cortrace decode` | run `cortrace-decode` (ETM/DWT/ITM -> Perfetto) directly, arguments passed through |
| `cortrace tcbmap` | read the live thread-name map |
| `cortrace arm` | arm ETM/ITM/DWT through a running OpenOCD (chips live in `platforms/`) |
| `cortrace os` | OS plugins (NuttX and FreeRTOS built in): list, find the watched variable, resolve thread names |
| `cortrace fpga ctrl\|net\|health` | CSR control, link discovery and health readout of the capture appliance |
| `cortrace open` | open a trace file in ui.perfetto.dev |

**The output directory is always explicit**: `--out-dir DIR` or `export CORTRACE_OUT_DIR=DIR`. A
capture streams ~75 MB/s and the decoded files are ~5x the raw size (about 450 MB per second of
trace), so cortrace never picks a location for you; it estimates the space needed before writing
anything and stops if the disk cannot hold it, and captures over 5 s need `--allow-long`. cortrace
never deletes files on its own.

```sh
export CORTRACE_OUT_DIR=~/traces
cortrace capture --iface enx0123 --elf fw.elf --secs 1 --width 4 --open
cortrace serve   --iface enx0123 --elf fw.elf          # then press Start in the web page
```

> Arm the target's ETM/DWT/ITM before capturing: with `cortrace arm` (through a running OpenOCD) or your own
> debugger, and keep it attached. Merging with NuttX's notes is done by `nxtrace merge` in pynuttx; cortrace does
> not depend on pynuttx.

**The full walkthrough (capture and decode, capture with notes, click-to-capture in the browser,
troubleshooting) is in [`docs/04-user-guide.md`](docs/04-user-guide.md)** (written in Chinese).

## Why

The trimmed-down ETMv4 decoders other projects use go wrong when capture
quality is marginal: on a corrupt byte they keep walking with a stale PC,
fabricating up to ~4.6x the real instruction count and hundreds of phantom
function re-entries. OpenCSD handles the same stream correctly and honestly
marks undecodable regions instead of inventing program flow. Cortrace
outsources the hardest part -- ETM decode -- to OpenCSD and owns only the
deterministic layer above it: call-stack reconstruction, time-base alignment,
and Perfetto export.

The prototype produced a call graph that matches the ELF **edge-for-edge
(0 mismatch)**, with nesting balanced. The current `cortrace-decode` CLI runs
the full offline pipeline (real OpenCSD library -> call-stack machine) and
reproduces that result on a CoreMark slice: **11/11 call edges against the ELF,
0 mismatch, begin/end balanced, 14 SysTick exceptions rendered**.

## Build from source

Requires CMake >= 3.16, a C++17 compiler, and optionally `libopencsd-dev` for
the decoder adapter. The core library + tests build without OpenCSD; with
OpenCSD present it additionally produces the `cortrace-decode` CLI.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Build the .deb yourself (inside an ubuntu:22.04 container so the runtime dependencies match that
release; the version comes from the latest `vX.Y.Z` tag):

```sh
scripts/build-deb.sh dist                       # produces dist/cortrace_<version>_amd64.deb
scripts/smoke-deb.sh dist/cortrace_*.deb        # clean container + ordinary user smoke test
```

Pushing a `v*` tag makes CI build the deb, smoke-test it and attach it to the GitHub release.

### Versioning

The single source is the `VERSION` file at the repository root (the first release is `1.0.0`),
a subset of [PEP 440](https://peps.python.org/pep-0440/): `MAJOR.MINOR.PATCH` with an optional
`aN`/`bN`/`rcN` (pre-release), `.postN` or `.devN` suffix, e.g. `1.0.0`, `1.0.0a1`, `1.0.0rc1`,
`1.0.0.post1`, `1.0.0.dev3`.

| Build | `cortrace version` / `cortrace-decode --version` |
|-------|--------------------------------------------------|
| HEAD is exactly the `v<VERSION>` tag (a release) | `1.0.0` |
| any other commit (a development build) | `1.0.0+git<commits>.<hash>` |
| running an unbuilt source tree | `1.0.0+dev` |

The deb uses the Debian spelling so `apt` orders versions like PEP 440 does: `1.0.0a1` becomes
`1.0.0~a1` (sorts before `1.0.0`), `1.0.0.post1` becomes `1.0.0+post1`. To release: edit
`VERSION`, commit, `git tag v<VERSION>`, push. CI checks that the tag matches `VERSION`, and
`a`/`b`/`rc`/`dev` tags are marked as pre-releases on the GitHub release.

## Offline decode (cortrace-decode)

Run the full pipeline on a deframed ETMv4 byte stream and report quality
metrics (balance, call edges). `mem.bin` is a flat image of the ELF and
`mem_base` is its lowest LMA (`08000000` on the STM32H743, typically).

```sh
arm-none-eabi-objcopy -O binary fw.elf mem.bin      # flat image
arm-none-eabi-nm fw.elf > syms.nm                   # symbol table
build/cortrace-decode capture.etm.bin mem.bin 08000000 syms.nm --edges edges.tsv
```

Criteria: `begins == ends` (balanced), and every edge in `edges.tsv`
corresponds to a real `bl` in the ELF (0 mismatch).

## Coverage

```sh
cmake -S . -B build -DENABLE_COVERAGE=ON
cmake --build build --target coverage   # runs tests + gcovr, fails below the gate
```

Set the gate threshold with `-DCORTRACE_MIN_COVERAGE=<percent>` (default 95).
The HTML report lands in `build/coverage-html/`.

## Formatting

WebKit-based clang-format (see `.clang-format`).

```sh
scripts/format.sh          # C/C++ format in place (clang-format)
scripts/format.sh --check  # CI mode: fail on any diff
```

The Python package (`python/cortrace`) uses black + pylint (see `pyproject.toml`, `.pylintrc`):

```sh
scripts/format-py.sh          # black format in place
scripts/format-py.sh --check  # CI mode: black --check + pylint
python -m pytest python/tests --cov --cov-fail-under=90   # tests + coverage gate (90%)
```

## Git hooks

Enforce the format check on every commit:

```sh
scripts/install-hooks.sh   # sets core.hooksPath = .githooks
```

The `pre-commit` hook **blocks the commit** whenever any staged C/C++ source
does not conform to `.clang-format`, or any staged Python fails black/pylint. The `commit-msg` hook
requires subjects of the form `type(scope): description`; only `docs` may omit the scope.

## Layout

```
include/cortrace/   Public headers (element / symbols / callstack ...)
src/                Core implementation (no OpenCSD dependency)
tools/              cortrace-decode, cortrace-cov, cortrace-prof (C++), cortrace-grab (C, FPGA UDP capture)
python/cortrace/    the cortrace command line and Python package (capture / serve / tcbmap / fpga ...)
python/tests/       Python unit tests
tests/              Zero-dependency C++ unit tests + framework
cmake/              CodeCoverage.cmake (gcovr + gate), Packaging.cmake (install + deb)
packaging/debian/   deb postinst / prerm
scripts/            format*.sh, install-hooks.sh, build-deb.sh, smoke-deb.sh
.githooks/          pre-commit (format/lint gate), commit-msg (subject format)
.github/workflows/  ci.yml (format + build + test + coverage + Python gate + deb)
docs/               design docs (architecture, Perfetto bridges, fusion, ...)
```

## Related repos

- [**cortrace-fpga**](https://github.com/cortrace/cortrace-fpga) — RTL of the Artix-7 parallel-ETM capture appliance plus small board-debug tools (the formal PC-side tools live in this repo)
- [**stm32h743-etm-trace-firmware**](https://github.com/cortrace/stm32h743-etm-trace-firmware) — deterministic selftrace target firmware

## License

MIT © 2026 VIFEX (see [`LICENSE`](LICENSE)). The statically linked OpenCSD is BSD-3-Clause (see
`/usr/share/doc/cortrace/LICENSE.opencsd` in the package).
