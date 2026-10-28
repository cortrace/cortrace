"""capture -- grab a raw trace from the FPGA streamer, then decode it.

  1. (optional) set the TPIU port width on the FPGA        (fpga.ctrl set-width)
  2. stream N seconds of UDP trace into a file              (cortrace-grab)
  3. decode raw -> Perfetto                                 (cortrace-decode)
     --notes: also write the software trace bytes that ride on an ITM stimulus
              port (notes_<tag>.bin), the input of `nxtrace merge` next to
              hw_<tag>.perfetto, whose Threads track is the thread switch log
  4. optionally open the result in ui.perfetto.dev

The output directory is always given by the user (--out-dir or
$CORTRACE_OUT_DIR): a 75 MB/s capture plus its decode is several hundred MB per
second of trace, so cortrace never picks a place on its own.

This tool does not arm the target's trace hardware (ETM/DWT/ITM); do that with
your debugger first and leave it attached.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

from . import osctl, osplugins, runstore
from ._paths import find_binary
from .elfinfo import ElfError
from .fpga import ctrl
from .osplugins import threadmap
from .perfetto_open import main as perfetto_open_main
from .probe import ProbeError

DEFAULT_TSGEN_HZ = 75e6  # ETM timestamp clock of the STM32H7 bring-up (HCLK)


def add_capture_options(ap):
    """Options shared by `capture` and `serve` (everything but --raw-in/--open)."""
    ap.add_argument(
        "--out-dir",
        default=os.environ.get("CORTRACE_OUT_DIR"),
        help="directory for raw captures and Perfetto files (required; "
        "default: $CORTRACE_OUT_DIR)",
    )
    ap.add_argument("--elf", required=True, help="firmware ELF")
    ap.add_argument(
        "--iface", default=os.environ.get("CORTRACE_IFACE"), help="capture NIC"
    )
    ap.add_argument("--secs", type=float, default=1.0, help="capture seconds")
    ap.add_argument(
        "--allow-long",
        action="store_true",
        help=f"allow captures longer than {runstore.MAX_SECS:g} s (~450 MB per second)",
    )
    ap.add_argument(
        "--width",
        type=int,
        choices=(4, 2, 1),
        default=None,
        help="set the FPGA TPIU port width before capture (default: leave as-is)",
    )
    ap.add_argument("--tag", default=None, help="file name suffix (default: timestamp)")
    ap.add_argument(
        "--notes",
        action="store_true",
        help="also write notes_<tag>.bin (the bytes on the ITM stimulus port) "
        "for `nxtrace merge` (the thread switches it needs are in the hardware "
        "trace's Threads track); uses the hybrid time base",
    )
    ap.add_argument(
        "--itm-port",
        type=int,
        default=1,
        help="ITM stimulus port the firmware writes its notes to (default: "
        "%(default)s; the Vela coresight note driver uses 0)",
    )
    ap.add_argument("--nm", default="arm-none-eabi-nm", help="nm for the ELF symbols")
    ap.add_argument(
        "--time-base",
        choices=("cycle", "etm", "hybrid"),
        default="cycle",
        help="time base of the hardware trace (--notes always uses hybrid)",
    )
    ap.add_argument("--sysclk-hz", type=float, default=150e6)
    ap.add_argument(
        "--tsgen-hz",
        type=float,
        default=None,
        help="ETM timestamp clock (needed by the etm and hybrid time bases; "
        f"--notes defaults to {DEFAULT_TSGEN_HZ:g})",
    )
    ap.add_argument(
        "--switch-stream",
        "--nx-switch-stream",
        dest="switch_stream",
        type=int,
        default=1,
        help="TPIU stream with the DWT current-task writes (default: %(default)s)",
    )
    ap.add_argument(
        "--os",
        default="auto",
        help="OS plugin that names the threads: a plugin name (see `cortrace os "
        "list`), 'auto' to detect it from the ELF, or 'none' (default: auto)",
    )
    ap.add_argument(
        "--probe-telnet",
        default=None,
        metavar="HOST:PORT",
        help="read live target memory (task names) through a running OpenOCD",
    )
    ap.add_argument(
        "--tcbmap",
        default=None,
        help="thread map that overrides the plugin's answers (cortrace tcbmap)",
    )
    ap.add_argument("--phase", default=None, help="lock deframe phase, e.g. 1,0")


def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        prog="cortrace capture",
        description="capture -> decode -> open in Perfetto",
    )
    add_capture_options(ap)
    ap.add_argument(
        "--raw-in", default=None, help="skip the capture; use this raw file"
    )
    ap.add_argument("--open", action="store_true", help="open the result in Perfetto")
    return finish_args(ap, ap.parse_args(argv))


def finish_args(ap, a):
    """Validate shared options and fill derived ones."""
    if not a.out_dir:
        ap.error("--out-dir is required (or set $CORTRACE_OUT_DIR)")
    if not getattr(a, "raw_in", None) and not a.iface:
        ap.error("capture NIC not given: pass --iface or set $CORTRACE_IFACE")
    a.out_dir = os.path.abspath(a.out_dir)
    a.elf = os.path.abspath(a.elf)
    a.tag = a.tag or time.strftime("%Y%m%d-%H%M%S")
    return a


def log(msg):
    print(f"[cortrace-capture] {msg}", file=sys.stderr)


def run_cmd(cmd, **kw):
    log("$ " + " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=False, **kw)


GRAB_LOST_DATA = 1  # cortrace-grab: file written, but UDP frames were lost


def grab(a, raw, allow_loss=False):
    """Stream a.secs seconds from the FPGA into `raw`. With `allow_loss`, a
    capture that lost UDP frames is kept (the coverage decoder drops the
    damaged trace segments); returns False in that case."""
    if a.width is not None:
        iface_args = ["--iface", a.iface] if a.iface else []
        if ctrl.main(iface_args + ["set-width", str(a.width)]) != 0:
            sys.exit("set-width failed")
    grab_bin = find_binary("CORTRACE_GRAB", "cortrace-grab")
    cmd = [grab_bin, a.iface, str(a.secs), raw, "256", "512"]
    code = run_cmd(cmd).returncode
    written = os.path.isfile(raw) and os.path.getsize(raw) > 0
    if code == GRAB_LOST_DATA and allow_loss and written:
        log("warning: UDP frames were lost in the capture (see seq-gap above)")
        return False
    if code != 0:
        sys.exit("cortrace-grab failed")
    return True


def note_path(a):
    """The note stream written by --notes."""
    return os.path.join(a.out_dir, f"notes_{a.tag}.bin")


def time_base_args(a):
    """cortrace-decode options of the time base."""
    base = "hybrid" if a.notes else a.time_base
    tsgen = a.tsgen_hz or (DEFAULT_TSGEN_HZ if a.notes else None)
    if base == "cycle":
        return ["--cycle-time", "--sysclk-hz", str(a.sysclk_hz)]
    if not tsgen:
        sys.exit(f"--time-base {base} needs --tsgen-hz")
    if base == "etm":
        return ["--etm-time", "--tsgen-hz", str(tsgen)]
    return [
        "--hybrid-time",
        "--tsgen-hz",
        str(tsgen),
        "--sysclk-hz",
        str(a.sysclk_hz),
    ]


def base_command(a, raw, syms):
    """cortrace-decode command line up to the part every mode shares."""
    cmd = [
        find_binary("CORTRACE_DECODE", "cortrace-decode"),
        raw,
        syms,
        "--elf",
        a.elf,
        "--raw",
        "--trace-width",
        str(a.width or 4),
        "--memory-limit-mb",
        "0",
        "--switch-stream",
        str(a.switch_stream),
    ]
    if a.phase:
        cmd += ["--phase", a.phase]
    return cmd


def decode_command(a, raw, perf, syms, thread_map=None):
    """cortrace-decode command line."""
    cmd = base_command(a, raw, syms)
    cmd += time_base_args(a) + ["--perf", perf]
    if a.notes:
        # The notes are stamped with the 32-bit DWT_CYCCNT; the decoder widens
        # it to 64 bits across counter wraps (every ~28.6 s at 150 MHz).
        cmd += ["--itm-note-port", str(a.itm_port), "--itm-note-unwrap"]
        cmd += ["--itm-note-out", note_path(a)]
    if thread_map:
        cmd += ["--thread-map", thread_map]
    return cmd


def build_thread_map(a, raw, syms):
    """Name the threads of this capture: ask the decoder which pointers the DWT
    saw, let the OS plugin resolve them, and write the thread map. Returns the
    map's path."""
    values = os.path.join(a.out_dir, f"switches_{a.tag}.txt")
    cmd = base_command(a, raw, syms) + ["--dump-switch-values", values]
    if run_cmd(cmd, capture_output=True, text=True).returncode != 0:
        sys.exit("could not read the thread switches from the capture")
    extra = threadmap.read_map(a.tcbmap) if a.tcbmap else None
    try:
        threads = osctl.resolve_threads(
            a.elf, a.os, threadmap.read_values(values), a.probe_telnet, extra
        )
    except (osplugins.PluginError, ElfError, ProbeError) as exc:
        sys.exit(f"thread names: {exc}")
    path = os.path.join(a.out_dir, f"threads_{a.tag}.txt")
    threadmap.write_map(path, threads)
    os.remove(values)
    return path


def decode_hw(a, raw):
    """Decode `raw`; returns the hardware Perfetto path."""
    perf = os.path.join(a.out_dir, f"hw_{a.tag}.perfetto")
    syms = os.path.join(tempfile.gettempdir(), f"cortrace_capture_{os.getpid()}.nm")
    with open(syms, "w", encoding="utf-8") as f:
        subprocess.run([a.nm, "-n", a.elf], stdout=f, check=True)
    tmap = build_thread_map(a, raw, syms)
    cmd = decode_command(a, raw, perf, syms, tmap)
    proc = run_cmd(cmd, capture_output=True, text=True)
    keys = ("stream health", "begins / ends", "wrote", "error", "itm notes: port")
    for line in (proc.stderr + proc.stdout).splitlines():
        if any(k in line for k in keys):
            log("  " + line.strip())
    if proc.returncode != 0 or not os.path.isfile(perf):
        sys.exit("decode failed")
    if a.notes:
        notes = note_path(a)
        if not os.path.isfile(notes) or os.path.getsize(notes) == 0:
            log(
                f"warning: no note bytes on ITM port {a.itm_port} -- does the "
                "firmware write its notes there, and is the ITM enabled?"
            )
        log(f"notes: {notes}")
    return perf


def run(a, raw_in=None):
    """Run the whole pipeline; returns the hardware Perfetto path."""
    if raw_in:
        raw = os.path.abspath(raw_in)
        if not os.path.isfile(raw):
            sys.exit(f"--raw-in not found: {raw}")
        need = runstore.estimate_decode_bytes(os.path.getsize(raw))
        what = "decoding the capture"
    else:
        runstore.check_duration(a.secs, a.allow_long)
        need = runstore.estimate_capture_bytes(a.secs)
        what = f"a {a.secs:g} s capture and its decode"
    runstore.require_space(a.out_dir, need, what)
    os.makedirs(a.out_dir, exist_ok=True)
    if not raw_in:
        raw = os.path.join(a.out_dir, f"raw_{a.tag}.bin")
        grab(a, raw)
    return decode_hw(a, raw)


def main(argv=None):
    a = parse_args(argv)
    result = run(a, raw_in=a.raw_in)
    print(result)
    if a.open:
        return perfetto_open_main([result, "--keep"])
    return 0
