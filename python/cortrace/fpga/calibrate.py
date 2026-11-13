"""calibrate -- find the sampling point that decodes cleanly, and set it.

`cortrace fpga ctrl calibrate --elf fw.elf` moves one of the FPGA's sampling
knobs, captures a short stretch of the running target at each setting, decodes it
with cortrace-decode and keeps the settings that decode without sampling errors.
It writes the middle of the longest such window to the FPGA.

The knob depends on the sampling clock of the bitstream:
  oversample  the sample point of the oversampling capture, in 200 MHz cycles (5 ns)
          after each TRACECLK edge. For targets whose data changes WITH the edge,
          where no IDELAY tap or MMCM phase can sample: at 12 MHz half a period is
          41 ns. Needs the bitstream feature; `--mode auto` falls back to it when
          the IDELAY / MMCM scan finds no window.
  direct  the IDELAY tap of the data lanes, 0..31 (~78 ps each); not circular
  mmcm    the fine phase of the MMCM clock, in 1/56 of a VCO period. One bit time
          is 28 x MMCM_MULT steps whatever the clock, so the settings wrap round
          every `period` steps and the window is an arc of that circle.

A setting passes when the decode shows no sampling errors:
  E = max(0, lost-sync - resync(TraceOn)) + addr-nacc   must be <= --max-errors
  N = slice events                                       must be >= half the median
The lost-sync that comes with a resync(TraceOn) is the ETM overflowing the trace
port, not a sampling error. N catches the failure that shows no errors at all: a
decode that only sees synchronisation frames.

The target must be armed and running something whose trace changes all the time
(a loop that repeats the same few branches hides sampling errors).
"""

import json
import os
import re
import resource
import shlex
import subprocess
import sys
import tempfile
import time

from .. import prbs
from .._paths import find_binary

PHASE_STEPS_PER_BIT_PER_MULT = 28  # 56 steps per VCO period, 2 bits per TRACECLK
IDELAY_STEP_PS = 78.125
OS_CYCLE_PS = 5000.0  # one cycle of the 200 MHz oversampling clock
OS_MAX_DLY = 31
OS_SETTLE_CYCLES = 2  # a sample point this close to the next edge is not used
EXIT_OK, EXIT_ERROR, EXIT_NARROW, EXIT_NONE = 0, 1, 2, 3
# A decode of a few MB of trace needs well under 200 MB. The helpers run with an
# address-space limit so that a runaway one is stopped instead of the machine.
HELPER_MEMORY_BYTES = 2 << 30


def _limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (HELPER_MEMORY_BYTES, HELPER_MEMORY_BYTES))


_SUMMARY = {
    "slice_events": re.compile(r"slice events\s*:\s*(\d+)"),
    "lost_sync": re.compile(r"lost-sync=(\d+)"),
    "resync": re.compile(r"resync\(TraceOn\)=(\d+)"),
    "addr_nacc": re.compile(r"addr-nacc=(\d+)"),
}


def print_flush(*args, **kw):
    """print() that shows up at once when the output goes to a file."""
    kw.setdefault("flush", True)
    print(*args, **kw)


def parse_summary(text):
    """Numbers of a cortrace-decode summary, or None if there is none."""
    out = {}
    for key, rx in _SUMMARY.items():
        m = rx.search(text)
        if m is None:
            return None
        out[key] = int(m.group(1))
    return out


def sampling_errors(summary):
    """E: decode errors that are not the ETM overflowing the trace port."""
    return max(0, summary["lost_sync"] - summary["resync"]) + summary["addr_nacc"]


def reference_events(values):
    """N_ref: the 90th percentile of the event counts of the coarse pass. A window
    can take up less than half of the settings, so the median would be the count
    of a failing setting; the failures that matter are the ones with far fewer
    events than the best settings."""
    ordered = sorted(values)
    return ordered[int(0.9 * (len(ordered) - 1))]


# ---------------------------------------------------------------------------
# window search: pure functions, no hardware
# ---------------------------------------------------------------------------


class Window:  # pylint: disable=too-few-public-methods
    """A run of passing settings: `lo..hi` (unwrapped, hi >= lo), `width`, the
    middle `mid`, whether it is cut off by the end of a linear range (`edge`),
    whether the whole range passes (`full`), and how many separate runs there were."""

    def __init__(  # pylint: disable=too-many-arguments,too-many-positional-arguments
        self, lo, hi, edge=False, full=False, runs=1
    ):
        self.lo, self.hi = lo, hi
        self.width = hi - lo + 1
        self.mid = (lo + hi) // 2
        self.edge, self.full, self.runs = edge, full, runs


def _longest_run(flags, circular):
    """(start, length) of the longest run of True in `flags` (indices, cyclic if
    `circular`) and the number of runs; None if there is none."""
    n = len(flags)
    if not any(flags):
        return None
    runs = []
    if circular and all(flags):
        return (0, n), 1
    if circular:
        first_false = flags.index(False)
        order = [(first_false + 1 + k) % n for k in range(n)]
    else:
        order = list(range(n))
    cur_start, cur_len = None, 0
    for idx in order:
        if flags[idx]:
            if cur_len == 0:
                cur_start = idx
            cur_len += 1
        elif cur_len:
            runs.append((cur_start, cur_len))
            cur_len = 0
    if cur_len:
        runs.append((cur_start, cur_len))
    best = max(runs, key=lambda r: r[1])
    return best, len(runs)


def find_window(probe, lo, hi, step, circular=False):  # pylint: disable=too-many-locals
    """Search the settings lo..hi for the longest window that `probe` passes.

    `probe(p)` -> bool for an integer p; p may lie outside lo..hi when the range is
    circular (it is then taken modulo hi - lo + 1). A coarse pass tests every
    `step`-th setting, then both edges of the longest run are bisected to one
    setting. Returns (Window or None, {p: bool} of everything tested)."""
    period = hi - lo + 1
    seen = {}

    def test(p):
        key = lo + (p - lo) % period if circular else p
        if key not in seen:
            seen[key] = bool(probe(key))
        return seen[key]

    coarse = list(range(lo, hi + 1, step))
    if not circular and coarse[-1] != hi:
        coarse.append(hi)
    flags = [test(p) for p in coarse]
    found = _longest_run(flags, circular)
    if found is None:
        return None, seen
    (start, length), runs = found
    n = len(coarse)
    if length == n:  # every tested setting passes: no edge to find
        return Window(lo, hi, full=True, runs=runs), seen

    def unwrapped(k):  # position of the k-th coarse point after `start`
        i = start + k
        return coarse[i % n] + (period if i >= n and circular else 0)

    first = unwrapped(0)
    last = unwrapped(length - 1)
    edge = False
    # left edge
    if not circular and start == 0:
        left, edge = coarse[0], True
    else:
        prev = coarse[(start - 1) % n] - (period if circular and start == 0 else 0)
        fail, ok = prev, first
        while ok - fail > 1:
            mid = (fail + ok) // 2
            if test(mid):
                ok = mid
            else:
                fail = mid
        left = ok
    # right edge
    if not circular and start + length == n:
        right, edge = coarse[-1], True
    else:
        nxt = unwrapped(length)
        ok, fail = last, nxt
        while fail - ok > 1:
            mid = (ok + fail) // 2
            if test(mid):
                ok = mid
            else:
                fail = mid
        right = ok
    return Window(left, right, edge=edge, runs=runs), seen


def eye_text(seen, lo, hi):
    """One line per knob range: '#' passed, 'x' failed, '.' not tested."""
    return "".join(
        "#" if seen.get(p) else ("x" if p in seen else ".") for p in range(lo, hi + 1)
    )


# ---------------------------------------------------------------------------
# hardware
# ---------------------------------------------------------------------------


class CalibrationError(Exception):
    """Something other than the sampling point is wrong."""


class Prober:  # pylint: disable=too-many-instance-attributes
    """Sets a knob, captures, decodes: the `probe` of find_window."""

    def __init__(self, a, ctrl, health, iface):
        self.a, self.ctrl, self.health, self.iface = a, ctrl, health, iface
        self.log = lambda m: print_flush(m, file=sys.stderr)
        self.events = {}  # setting -> slice events
        self.rates = []  # MB/s of each capture; one byte per TRACECLK period
        tmp = tempfile.mkdtemp(prefix="cortrace_cal_", dir=os.environ.get("TMPDIR"))
        self.tmp = tmp
        self.raw = os.path.join(tmp, "cal.bin")
        self.syms = os.path.join(tmp, "cal.nm")
        with open(self.syms, "w", encoding="utf-8") as f:
            subprocess.run([a.nm, "-n", a.elf], stdout=f, check=True)

    def close(self):
        """Remove the temporary files."""
        for name in os.listdir(self.tmp):
            os.remove(os.path.join(self.tmp, name))
        os.rmdir(self.tmp)

    def grab(self, secs):
        """Capture `secs` of the stream; returns the bytes written."""
        cmd = [
            find_binary("CORTRACE_GRAB", "cortrace-grab"),
            self.iface,
            str(secs),
            self.raw,
            "256",
            "512",
        ]
        try:
            subprocess.run(
                cmd,
                check=False,
                capture_output=True,
                timeout=30,
                preexec_fn=_limit_memory,
            )
        except subprocess.TimeoutExpired as exc:
            raise CalibrationError(
                "cortrace-grab got no data (is the FPGA streaming?)"
            ) from exc
        return os.path.getsize(self.raw) if os.path.isfile(self.raw) else 0

    def decode(self):
        """Summary of cortrace-decode on the last capture, or None."""
        width = str(getattr(self.a, "width", 4))
        if getattr(self.a, "prbs", False):
            return self.decode_prbs(width)
        cmd = [
            find_binary("CORTRACE_DECODE", "cortrace-decode"),
            self.raw,
            self.syms,
            "--elf",
            self.a.elf,
            "--raw",
            "--trace-width",
            width,
            "--memory-limit-mb",
            "512",
            "--cycle-time",
            "--sysclk-hz",
            "150000000",
        ] + shlex.split(self.a.decode_args or "")
        try:
            done = subprocess.run(
                cmd,
                check=False,
                capture_output=True,
                text=True,
                timeout=300,
                preexec_fn=_limit_memory,
            )
            out = done.stdout + done.stderr  # the summary goes to stderr
        except subprocess.TimeoutExpired:
            return None
        return parse_summary(out)

    def decode_prbs(self, width):
        """Score of the ITM PRBS stream (see prbs.py) in the last capture, in the
        shape of a decode summary, or None if the decode did not run."""
        dump = os.path.join(self.tmp, "stream1.bin")
        cmd = [
            find_binary("CORTRACE_DECODE", "cortrace-decode"),
            self.raw,
            self.syms,
            "--elf",
            self.a.elf,
            "--raw",
            "--trace-width",
            width,
            "--want-stream",
            "1",
            "--etm-trace-id",
            "1",
            "--dump-etm",
            dump,
            "--memory-limit-mb",
            "512",
        ]
        if os.path.isfile(dump):
            os.remove(dump)
        try:
            subprocess.run(
                cmd,
                check=False,
                capture_output=True,
                timeout=300,
                preexec_fn=_limit_memory,
            )
        except subprocess.TimeoutExpired:
            return None
        if not os.path.isfile(dump):
            return None
        with open(dump, "rb") as f:
            return prbs.summary(f.read())

    @property
    def rate_mhz(self):
        """Pin clock estimate: the median capture rate. (The first capture after
        a re-arm can be a burst out of the DDR ring, so not the maximum.)"""
        return sorted(self.rates)[len(self.rates) // 2] if self.rates else 0.0

    def measure(self, secs):
        """Capture and decode; a capture right after a change is thrown away."""
        self.grab(0.02)
        size = self.grab(secs)
        self.rates.append(size / secs / 1e6)
        return self.decode()


class Knob:  # pylint: disable=too-few-public-methods
    """A setting that moves the sampling point."""

    name = "?"
    circular = False
    lo, hi = 0, 0

    def apply(self, value):
        """Write the setting to the FPGA."""
        raise NotImplementedError


class IdelayKnob(Knob):  # pylint: disable=too-few-public-methods
    """The IDELAY tap of the data lanes."""

    name = "tap"
    lo, hi = 0, 31
    unit_ps = IDELAY_STEP_PS

    def __init__(self, ip, ctrl):
        self.ip, self.ctrl = ip, ctrl

    def apply(self, value):
        self.ctrl.write_csr(self.ip, self.ctrl.REG_IDELAY_ALL, value)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_REARM, 1)


class OversampleKnob(Knob):
    """The sample point of the oversampling capture, 200 MHz cycles after the
    edge. The range ends before the next edge: a clock with half a period of
    H cycles allows 0..H-2."""

    name = "dly"
    unit_ps = OS_CYCLE_PS
    lo = 0

    def __init__(self, ip, ctrl, half_cycles, vote):
        self.ip, self.ctrl = ip, ctrl
        self.half = half_cycles
        self.hi = max(1, min(OS_MAX_DLY, int(half_cycles) - OS_SETTLE_CYCLES))
        self.blank = max(1, min(OS_MAX_DLY, int(half_cycles) // 3))
        self.vote = vote

    def prepare(self):
        """Switch the capture to oversampling."""
        self.ctrl.write_csr(self.ip, self.ctrl.REG_OS_BLANK, self.blank)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_OS_CLR, 1)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_OS_CTRL, 1 | (2 if self.vote else 0))

    def release(self):
        """Back to the IDDR capture."""
        self.ctrl.write_csr(self.ip, self.ctrl.REG_OS_CTRL, 0)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_REARM, 1)

    def apply(self, value):
        self.ctrl.write_csr(self.ip, self.ctrl.REG_OS_DLY, value)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_REARM, 1)


class PhaseKnob(Knob):
    """The fine phase of the MMCM sampling clock; wraps every `period` steps."""

    name = "phase"
    circular = True

    def __init__(self, ip, ctrl, health, mult):
        self.ip, self.ctrl, self.health = ip, ctrl, health
        self.period = PHASE_STEPS_PER_BIT_PER_MULT * mult
        self.lo = -(self.period // 2)
        self.hi = self.lo + self.period - 1
        self.unit_ps = None  # known once the pin clock is

    def hw_value(self, value):
        """The setting inside the hardware's +-255 steps that equals `value`."""
        v = (value - self.lo) % self.period + self.lo
        return v

    def apply(self, value):
        v = self.hw_value(value)
        raw = v & 0x3FF
        self.ctrl.write_csr(self.ip, self.ctrl.REG_MM_PHASE_LO, raw & 0xFF)
        self.ctrl.write_csr(self.ip, self.ctrl.REG_MM_PHASE_HI, raw >> 8)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                locked, busy, _t, phase, _l = self.health.read_mmcm_status(self.ip)
            except OSError:
                continue
            if not locked:
                raise CalibrationError("the sampling MMCM is not locked (no TRACECLK?)")
            if not busy and phase == v:
                break
            time.sleep(0.02)
        else:
            raise CalibrationError("the sampling MMCM did not reach the phase")
        self.ctrl.write_csr(self.ip, self.ctrl.REG_REARM, 1)


def _clock_source(ip, health):
    """'direct' or 'mmcm': what the bitstream samples with now."""
    import socket  # pylint: disable=import-outside-toplevel

    health.IP = ip
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2.0)
    try:
        ident = health.Identity(s)
        if not ident.mmcm:
            return "direct"
        if not ident.dual:
            return "mmcm"
        return "mmcm" if health.rd8(s, health.A_CLK_SEL) & 1 else "direct"
    finally:
        s.close()


def add_arguments(p):
    """Options of `cortrace fpga ctrl calibrate`."""
    p.add_argument("--elf", required=True, help="ELF of the firmware that is running")
    p.add_argument("--nm", default="arm-none-eabi-nm", help="nm for the ELF symbols")
    p.add_argument("--secs", type=float, default=0.05, help="capture per setting")
    p.add_argument(
        "--width",
        type=int,
        default=4,
        choices=(1, 2, 4),
        help="TPIU port width of the stream (default 4)",
    )
    p.add_argument(
        "--prbs",
        action="store_true",
        help="judge a setting by the ITM PRBS stream of tools/prbs_itm (start it "
        "with `cortrace prbs start`; --elf is its prbs_itm.elf) instead of the ETM "
        "decode: random data finds sampling errors that ETM data hides",
    )
    p.add_argument(
        "--mode",
        choices=("auto", "direct", "mmcm", "oversample"),
        default="auto",
        help="knob to turn: the IDELAY tap (direct), the MMCM phase (mmcm) or the "
        "sample point of the oversampling capture (oversample); auto starts with "
        "what the bitstream samples with now and, if that finds no window, tries "
        "oversampling when the bitstream has it",
    )
    p.add_argument(
        "--vote",
        action="store_true",
        help="oversampling: take the majority of three samples",
    )
    p.add_argument(
        "--decode-args",
        default="",
        help="extra options for cortrace-decode, e.g. the ETM ID registers of the "
        "target ('--etm-idr0 0x08020EFF --etm-configr 0x1')",
    )
    p.add_argument(
        "--coarse-step", type=int, default=None, help="default: 4 taps, 28 phase steps"
    )
    p.add_argument("--max-errors", type=int, default=0)
    p.add_argument(
        "--min-width",
        type=int,
        default=None,
        help="narrowest window to accept, in settings (default: 6 taps, ~0.5 ns of phase)",
    )
    p.add_argument(
        "--mmcm-mult", type=int, default=14, help="VCO multiplier of the bitstream"
    )
    p.add_argument("--no-apply", action="store_true", help="measure, do not write")
    p.add_argument("--save", default=None, help="record the result here (JSON)")
    p.add_argument("--name", default="default", help="name of the default record")


def default_record_path(name):
    """Where the result is recorded unless --save says otherwise."""
    base = os.environ.get(
        "CORTRACE_CALIBRATION_DIR",
        os.path.join(os.path.expanduser("~"), ".config", "cortrace", "calibration"),
    )
    return os.path.join(base, f"{name}.json")


def _unit_ps(knob, rate_mhz, mult):
    if knob.name == "tap":
        return IDELAY_STEP_PS
    if knob.name == "dly":
        return OS_CYCLE_PS
    if not rate_mhz:
        return None
    return 1e6 / (56 * rate_mhz * mult)


def _make_knob(a, mode, ip, ctrl, health):
    if mode == "direct":
        return IdelayKnob(ip, ctrl)
    if mode == "oversample":
        st = health.read_os_status(ip)
        if st["half_cycles"] is None:
            raise CalibrationError(
                "the oversampler cannot measure TRACECLK (no clock, or slower than "
                "0.8 MHz): is the target tracing?"
            )
        if st["half_cycles"] < OS_SETTLE_CYCLES + 1:
            raise CalibrationError(
                f"TRACECLK is too fast for oversampling ({st['pin_mhz']:.1f} MHz; "
                "the limit is about 33 MHz, where only dly 0..1 is left)"
            )
        return OversampleKnob(ip, ctrl, st["half_cycles"], a.vote)
    return PhaseKnob(ip, ctrl, health, a.mmcm_mult)


def run(
    a, ip, iface, ctrl, health
):  # pylint: disable=too-many-locals,too-many-return-statements,too-many-branches
    """Entry point of the subcommand; returns the exit code."""
    if not iface:
        print_flush("calibrate: pass --iface (the NIC that receives the FPGA's stream)")
        return EXIT_ERROR
    try:
        has_os = health.has_oversample(ip)
        if a.mode == "oversample" and not has_os:
            print_flush("calibrate: this bitstream has no oversampling capture")
            return EXIT_ERROR
        first = a.mode if a.mode != "auto" else _clock_source(ip, health)
        modes = [first]
        if a.mode == "auto" and has_os:
            modes.append("oversample")
        if has_os and first != "oversample":
            ctrl.write_csr(ip, ctrl.REG_OS_CTRL, 0)  # the IDDR path to start with
        prober = Prober(a, ctrl, health, iface)
    except (OSError, subprocess.CalledProcessError, CalibrationError) as exc:
        print_flush(f"calibrate: {exc}")
        return EXIT_ERROR
    code = EXIT_ERROR
    try:
        for i, mode in enumerate(modes):
            knob = None
            try:
                knob = _make_knob(a, mode, ip, ctrl, health)
                if mode == "oversample":
                    knob.prepare()
                step = a.coarse_step or _coarse_step(knob)
                code = _calibrate(a, knob, mode, step, prober)
            except CalibrationError as exc:
                print_flush(f"calibrate: {exc}")
                code = EXIT_ERROR
            if code == EXIT_OK:
                break
            if knob is not None and mode == "oversample":
                knob.release()  # not usable: leave the IDDR capture on
            if i + 1 < len(modes):
                print_flush(f"no {mode} setting works; trying {modes[i + 1]}")
        return code
    finally:
        prober.close()


def _coarse_step(knob):
    if knob.name == "tap":
        return 4
    if knob.name == "dly":
        return 1 if knob.hi <= 16 else 2
    return 28


def _calibrate(
    a, knob, mode, step, prober
):  # pylint: disable=too-many-locals,too-many-branches,too-many-statements
    results = {}
    print_flush(
        f"calibrating the {mode} sampling clock ({knob.name} knob, "
        f"{knob.lo}..{knob.hi}, coarse step {step})"
    )

    def measure(p):
        knob.apply(p)
        summary = prober.measure(a.secs)
        if summary is None or (
            summary["slice_events"] == 0 and sampling_errors(summary) == 0
        ):
            summary = prober.measure(
                a.secs
            )  # a capture right after a change can be empty
        if summary is None:
            summary = {
                "slice_events": 0,
                "lost_sync": 0,
                "resync": 0,
                "addr_nacc": 10**6,
            }
        results[p] = summary
        return summary

    # coarse pass first, to learn N_ref, then the real search reuses these
    coarse = list(range(knob.lo, knob.hi + 1, step))
    if not knob.circular and coarse[-1] != knob.hi:
        coarse.append(knob.hi)
    for p in coarse:
        measure(p)
    n_ref = reference_events([s["slice_events"] for s in results.values()])
    if n_ref < 100:
        print_flush(
            "calibrate: the decode finds almost no "
            + ("PRBS words" if getattr(a, "prbs", False) else "ETM data")
            + " at any setting. Is the target armed (cortrace arm"
            + (" --no-etm" if getattr(a, "prbs", False) else "")
            + ") and running "
            + (
                "`cortrace prbs start`?"
                if getattr(a, "prbs", False)
                else "the firmware of --elf?"
            )
        )
        return EXIT_ERROR

    def probe(p):
        s = results.get(p) or measure(p)
        passed = sampling_errors(s) <= a.max_errors and s["slice_events"] >= 0.5 * n_ref
        prober.events[p] = s["slice_events"]
        return passed

    window, seen = find_window(probe, knob.lo, knob.hi, step, circular=knob.circular)
    unit = _unit_ps(knob, prober.rate_mhz, a.mmcm_mult)
    print_flush(f"pin clock ~{prober.rate_mhz:.1f} MHz (from the capture rate)")
    print_flush(
        f"{knob.name:>5} {knob.lo:+d}{' ' * max(1, len(seen) // 4)}{knob.hi:+d}"
    )
    print_flush("      " + eye_text(seen, knob.lo, knob.hi))
    if window is None:
        print_flush(
            "no setting decodes cleanly. Try a lower pin clock, slower pads on the target, "
            "shorter or better grounded cable, or the other sampling clock "
            "(cortrace fpga ctrl set-mode)."
        )
        return EXIT_NONE
    width_text = f"{window.width} settings"
    if unit:
        width_text += f", {window.width * unit / 1000:.2f} ns"
    if window.full:
        chosen = 0 if knob.circular else window.mid
        print_flush(f"every setting passes ({width_text}); using {chosen}")
    else:
        chosen = window.mid
        note = ""
        if window.edge:
            note += " (cut off by the end of the range)"
        if window.runs > 1:
            note += f" ({window.runs} separate windows, the longest is used)"
        print_flush(
            f"window {window.lo}..{window.hi} ({width_text}){note}, chosen {chosen}"
        )
    min_width = a.min_width
    if min_width is None:
        if mode == "oversample":
            min_width = 2
        else:
            min_width = 6 if mode == "direct" else (round(500 / unit) if unit else 20)
    if not window.full and window.width < min_width:
        print_flush(
            f"the window is narrower than {min_width} settings; temperature and "
            "voltage drift could close it. Not applied."
        )
        return EXIT_NARROW
    if a.no_apply:
        print_flush("--no-apply: nothing written")
    else:
        knob.apply(chosen)
        s = prober.measure(max(a.secs, 0.3))
        if s is None or sampling_errors(s) > a.max_errors:
            print_flush(f"verification at {chosen} failed: {s}")
            return EXIT_ERROR
        print_flush(
            f"set {knob.name} = {knob.hw_value(chosen) if knob.circular else chosen}; "
            f"verified over {max(a.secs, 0.3):g} s ({s['slice_events']} slice events, "
            "no sampling errors)"
        )
    record = {
        "mode": mode,
        "knob": knob.name,
        "value": knob.hw_value(chosen) if knob.circular else chosen,
        "window": [window.lo, window.hi],
        "width": window.width,
        "width_ns": round(window.width * unit / 1000, 3) if unit else None,
        "pin_mhz": round(prober.rate_mhz, 1),
        **(
            {"blank": knob.blank, "vote": knob.vote, "half_cycles": knob.half}
            if mode == "oversample"
            else {}
        ),
        "time": time.strftime("%Y-%m-%d %H:%M:%S %z"),
        "applied": not a.no_apply,
    }
    path = a.save or default_record_path(a.name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    print_flush(f"recorded in {path}")
    return EXIT_OK
