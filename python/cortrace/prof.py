"""cortrace profile -- cycle-accurate profile from the ETM cycle counts.

  run     capture (or take a raw file), run cortrace-prof, write prof_<tag>.json
          and print the report
  report  print a profile file as text (or JSON)
  diff    compare two profile files, e.g. before and after a change, or with
          the caches on and off

The ETM must be armed with cycle counting: `cortrace arm --profile timing`.
What the numbers are, and what they are not:

  * cycles are exact for a function or block that is reported on its own
    between two cycle counts, and shared by instruction count when several
    ranges sit in one window. Compare functions in groups, not one by one,
    when they call each other all the time;
  * the cycle counts have a resolution of the ETM's threshold (16 cycles by
    default), so single instructions cannot be told apart;
  * a stall shows as cycles, not as its cause: to tell cache misses from flash
    waits, run the same load twice with one thing changed and `diff`.

Exit codes: 0 fine, 2 usage or input error.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

from . import capture, runstore
from ._paths import find_binary

ERROR = 2
FORMAT = "cortrace-prof"
VERSION = 1
EVENT_UNIT = 256  # events one wrap of a DWT counter stands for
EVENT_KINDS = ("cpi", "exc", "sleep", "lsu", "fold")


class ProfError(Exception):
    """A problem the user can fix (bad input, tool failure)."""


def log(msg):
    print(f"[cortrace-profile] {msg}", file=sys.stderr)


# ---- profile file -------------------------------------------------------------
def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        raise ProfError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("format") != FORMAT:
        raise ProfError(f"{path} is not a cortrace-prof file")
    if data.get("version") != VERSION:
        raise ProfError(
            f"{path}: profile version {data.get('version')}, expected {VERSION}"
        )
    return data


# ---- text -----------------------------------------------------------------------
def num(value):
    return f"{int(round(value)):,}"


def micros(cycles, hz):
    """' (12.3 us)' when the CPU clock is known, else ''."""
    if not hz:
        return ""
    return f" ({cycles / hz * 1e6:.2f} us)"


def cpi(entry):
    return entry["cycles"] / entry["instructions"] if entry["instructions"] else 0.0


def health_line(prof):
    health = prof["health"]
    losses = health["nosync"] + health["overflow"]
    if not losses:
        return "clean"
    return (
        f"{losses} losses (lost sync {health['nosync']}, overflow {health['overflow']}): "
        "the cycles across a loss are not attributed to code"
    )


def events_of(prof):
    """The DWT event block, or None when the trace had no event packets."""
    ev = prof.get("events")
    return ev if ev and ev["packets"] else None


def drift_line(prof):
    clock = prof["clock"]
    if clock["drift_ppm"] is None:
        return "not checked (give --sysclk-hz and --tsgen-hz)"
    ev = events_of(prof)
    asleep = " (the cycles asleep added)" if ev and ev["sleep_cycles"] else ""
    return f"{clock['drift_ppm']:+.1f} ppm against the global timestamps{asleep}"


def event_cycles(entry, kind):
    """Cycles of a DWT counter in a function: each wrap is 256 events."""
    return entry["events"][kind] * EVENT_UNIT


def load_line(prof):
    ev = events_of(prof)
    busy = prof["clock"]["cycles"]
    if ev["sleep_cycles"] == 0:
        return "never asleep (or the SLEEP counter is off)"
    pct = busy / (busy + ev["sleep_cycles"]) * 100
    return (
        f"{pct:.1f}% (busy {num(busy)} cycles, asleep {num(ev['sleep_cycles'])} cycles)"
    )


def text_events(prof):
    ev = events_of(prof)
    if not ev:
        return []
    wraps = ev["wraps"]
    kinds = ", ".join(f"{k} {num(wraps[k])}" for k in EVENT_KINDS)
    return [
        f"  DWT events        : {num(ev['packets'])} packets, wraps of {ev['unit']} ({kinds})",
        f"  CPU load          : {load_line(prof)}",
    ]


def text_header(prof):
    clk = prof["clock"]
    hz = clk["sysclk_hz"]
    secs = f" = {clk['cycles'] / hz:.4f} s at {hz / 1e6:g} MHz" if hz else ""
    ipc = clk["instructions"] / clk["cycles"] if clk["cycles"] else 0.0
    cpi_all = clk["cycles"] / clk["instructions"] if clk["instructions"] else 0.0
    share = clk["no_instr_cycles"] / clk["cycles"] * 100 if clk["cycles"] else 0.0
    return [
        "Cortrace Cycle Profile",
        f"  elf               : {prof['elf']}",
        f"  cycles            : {num(clk['cycles'])}{secs}",
        f"  instructions      : {num(clk['instructions'])}  (CPI {cpi_all:.3f}, IPC {ipc:.3f})",
        f"  cycle clock       : {drift_line(prof)}",
        f"  cycles with no instruction: {share:.3f}% (exception entry, trace gaps)",
        f"  trace health      : {health_line(prof)}",
    ] + text_events(prof)


def text_functions(prof, top):
    clk = prof["clock"]
    total = clk["cycles"] or 1
    with_events = events_of(prof) is not None
    rows = ["", f"Functions by self cycles (top {top})"]
    head = f"  {'function':<34}{'entries':>11}{'instr':>14}{'cycles':>14}{'%':>8}{'CPI':>7}"
    if with_events:
        head += f"{'LSU%':>7}{'MC%':>7}{'fold%':>7}"
    rows.append(head)
    for fn in prof["functions"][:top]:
        row = (
            f"  {fn['name'][:33]:<34}{num(fn['entries']):>11}{num(fn['instructions']):>14}"
            f"{num(fn['cycles']):>14}{fn['cycles'] / total * 100:>7.2f}%{cpi(fn):>7.2f}"
        )
        if with_events:
            cyc = fn["cycles"] or 1
            fold = (
                event_cycles(fn, "fold") / fn["instructions"] * 100
                if fn["instructions"]
                else 0
            )
            row += (
                f"{event_cycles(fn, 'lsu') / cyc * 100:>7.1f}"
                f"{event_cycles(fn, 'cpi') / cyc * 100:>7.1f}{fold:>7.1f}"
            )
        rows.append(row)
    if with_events:
        rows.append(
            "  LSU%: extra cycles of loads and stores (DWT LSUCNT) of the function's cycles; "
            "MC%: DWT CPICNT, multi-cycle instructions,"
        )
        rows.append(
            "  only to compare functions (it adds up to more than cycles - instructions); "
            "fold%: folded instructions per instruction."
        )
    return rows


def text_exc_overhead(prof):
    ev = events_of(prof)
    if not ev or not ev["wraps"]["exc"]:
        return []
    total = ev["wraps"]["exc"]
    top = sorted(prof["functions"], key=lambda f: -f["events"]["exc"])[:5]
    rows = [
        "",
        f"Exception overhead (DWT EXCCNT): {num(total * EVENT_UNIT)} cycles, where it landed",
    ]
    for fn in top:
        if fn["events"]["exc"]:
            rows.append(
                f"  {fn['name'][:33]:<34}{num(event_cycles(fn, 'exc')):>12} cycles"
                f"{fn['events']['exc'] / total * 100:>7.1f}%"
            )
    return rows


def text_exceptions(prof):
    if not prof["exceptions"]:
        return []
    hz = prof["clock"]["sysclk_hz"]
    rows = ["", "Exceptions (cycles)"]
    rows.append(
        f"  {'exception':<14}{'count':>9}{'entry p50':>11}{'run p50':>10}{'run p99':>10}"
        f"{'run max':>10}"
    )
    for exc in prof["exceptions"]:
        dur, ent = exc["duration"], exc["entry"]
        rows.append(
            f"  {exc['name']:<14}{num(exc['count']):>9}{num(ent['p50']):>11}"
            f"{num(dur['p50']):>10}{num(dur['p99']):>10}{num(dur['max']):>10}"
            + micros(dur["max"], hz)
        )
    return rows


def text_periodic(prof):
    per = prof["periodic"]
    if not per["period_cycles"]:
        return ["", f"{per['name']}: fewer than two occurrences, no lateness"]
    hz = prof["clock"]["sysclk_hz"]
    how = "given" if per["period_given"] else "median interval"
    late = per["lateness"]
    rows = [
        "",
        f"{per['name']} lateness: {num(per['occurrences'])} occurrences, period "
        f"{num(per['period_cycles'])} cycles ({how})",
        f"  against the earliest occurrence, in cycles: p50 {num(late['p50'])}, "
        f"p90 {num(late['p90'])}, p99 {num(late['p99'])}, max "
        f"{num(late['max'])}{micros(late['max'], hz)}",
    ]
    if per["blockers"]:
        rows.append(
            f"  {'was waiting at':<35}{'count':>8}{'min':>9}{'mean':>9}{'max':>9}"
        )
        for blk in per["blockers"]:
            rows.append(
                f"  {blk['function'][:24]:<24} 0x{blk['address']:08x}{num(blk['count']):>8}"
                f"{num(blk['min']):>9}{num(blk['mean']):>9}{num(blk['max']):>9}"
            )
    return rows


def text_blocks(prof):
    if not prof["blocks"]:
        return []
    rows = ["", "Basic blocks that ran alone (cycles per run: the spread is the stall)"]
    rows.append(
        f"  {'function':<26}{'range':<19}{'count':>10}{'min':>6}{'p50':>6}{'p90':>6}"
        f"{'p99':>6}{'max':>6}"
    )
    for blk in prof["blocks"]:
        rows.append(
            f"  {blk['function'][:25]:<26}{blk['start']:08x}-{blk['end']:08x}  "
            f"{num(blk['count']):>10}{blk['min']:>6}{blk['p50']:>6}{blk['p90']:>6}"
            f"{blk['p99']:>6}{blk['max']:>6}"
        )
    return rows


def text(prof, top=25):
    rows = text_header(prof) + text_functions(prof, top) + text_exceptions(prof)
    rows += text_exc_overhead(prof) + text_periodic(prof) + text_blocks(prof)
    return "\n".join(rows) + "\n"


# ---- diff -------------------------------------------------------------------------
def ratio(a, b):
    return f"{b / a:.2f}x" if a else "-"


def diff_totals(pa, pb):
    ca, cb = pa["clock"], pb["clock"]
    cpi_a = ca["cycles"] / ca["instructions"] if ca["instructions"] else 0.0
    cpi_b = cb["cycles"] / cb["instructions"] if cb["instructions"] else 0.0
    return [
        "Cortrace Cycle Profile Diff  (A -> B)",
        f"  instructions : {num(ca['instructions'])} -> {num(cb['instructions'])}",
        f"  CPI          : {cpi_a:.3f} -> {cpi_b:.3f}  ({ratio(cpi_a, cpi_b)})",
        f"  trace health : A {health_line(pa)}; B {health_line(pb)}",
        "  (the same time was captured, so a slower B runs fewer instructions: compare CPI)",
    ]


def lsu_pct(entry):
    """LSU extra cycles as a share of the function's cycles, or None."""
    if not entry or not entry["cycles"]:
        return None
    return event_cycles(entry, "lsu") / entry["cycles"] * 100


def diff_functions(pa, pb, top):
    by_b = {fn["name"]: fn for fn in pb["functions"]}
    with_events = events_of(pa) is not None and events_of(pb) is not None
    rows = ["", f"Functions by cycles in A (top {top}): CPI"]
    head = f"  {'function':<34}{'A':>8}{'B':>8}{'B/A':>9}"
    if with_events:
        head += f"{'LSU% A':>9}{'LSU% B':>9}"
    rows.append(head)
    for fn in pa["functions"][:top]:
        other = by_b.get(fn["name"])
        cpi_b = f"{cpi(other):.2f}" if other and other["instructions"] else "-"
        row = (
            f"  {fn['name'][:33]:<34}{cpi(fn):>8.2f}{cpi_b:>8}"
            f"{ratio(cpi(fn), cpi(other)) if other and other['instructions'] else '-':>9}"
        )
        if with_events:
            la, lb = lsu_pct(fn), lsu_pct(other)
            row += f"{la:>9.1f}" if la is not None else f"{'-':>9}"
            row += f"{lb:>9.1f}" if lb is not None else f"{'-':>9}"
        rows.append(row)
    return rows


def diff_exceptions(pa, pb):
    by_b = {e["number"]: e for e in pb["exceptions"]}
    rows = []
    for exc in pa["exceptions"]:
        other = by_b.get(exc["number"])
        if not other:
            continue
        rows.append(
            f"  {exc['name']:<14}{num(exc['duration']['p50']):>9}{num(other['duration']['p50']):>9}"
            f"{ratio(exc['duration']['p50'], other['duration']['p50']):>9}"
            f"{num(exc['duration']['p99']):>9}{num(other['duration']['p99']):>9}"
        )
    if not rows:
        return []
    head = ["", "Exception run time (cycles)"]
    head.append(
        f"  {'exception':<14}{'A p50':>9}{'B p50':>9}{'B/A':>9}{'A p99':>9}{'B p99':>9}"
    )
    return head + rows


def diff_periodic(pa, pb):
    la, lb = pa["periodic"]["lateness"], pb["periodic"]["lateness"]
    if not (la["count"] and lb["count"]):
        return []
    return [
        "",
        f"{pa['periodic']['name']} lateness (cycles): "
        f"p50 {num(la['p50'])} -> {num(lb['p50'])}, p99 {num(la['p99'])} -> {num(lb['p99'])}, "
        f"max {num(la['max'])} -> {num(lb['max'])}",
    ]


def diff(pa, pb, top=25):
    rows = diff_totals(pa, pb) + diff_functions(pa, pb, top)
    rows += diff_exceptions(pa, pb) + diff_periodic(pa, pb)
    return "\n".join(rows) + "\n"


# ---- run ------------------------------------------------------------------------------
def parse_run_args(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace profile run",
        description="capture -> cortrace-prof -> profile file and report",
    )
    ap.add_argument(
        "--out-dir",
        default=os.environ.get("CORTRACE_OUT_DIR"),
        help="directory for the raw capture and prof_<tag>.json (required; "
        "default: $CORTRACE_OUT_DIR)",
    )
    ap.add_argument("--elf", required=True, help="firmware ELF the trace ran")
    ap.add_argument(
        "--iface", default=os.environ.get("CORTRACE_IFACE"), help="capture NIC"
    )
    ap.add_argument("--secs", type=float, default=1.0, help="capture seconds")
    ap.add_argument(
        "--allow-long",
        action="store_true",
        help=f"allow captures longer than {runstore.MAX_SECS:g} s (decoding takes "
        "about 6 s per captured second)",
    )
    ap.add_argument(
        "--width",
        type=int,
        choices=(4, 2, 1),
        default=None,
        help="set the FPGA TPIU port width before capture (default: leave as-is)",
    )
    ap.add_argument("--phase", default=None, help="lock deframe phase, e.g. 1,0")
    ap.add_argument("--tag", default=None, help="file name suffix (default: timestamp)")
    ap.add_argument("--nm", default="arm-none-eabi-nm", help="nm for the ELF symbols")
    ap.add_argument("--sysclk-hz", type=float, default=0.0, help="CPU clock")
    ap.add_argument(
        "--tsgen-hz",
        type=float,
        default=0.0,
        help="ETM timestamp clock; with --sysclk-hz the cycle clock is checked against it",
    )
    ap.add_argument(
        "--period-exception",
        type=int,
        default=15,
        metavar="N",
        help="exception whose lateness is measured (default: 15, SysTick)",
    )
    ap.add_argument(
        "--period-cycles",
        type=float,
        default=0.0,
        help="its period in cycles (default: the median interval)",
    )
    ap.add_argument("--top", type=int, default=25, help="functions in the report")
    ap.add_argument(
        "--top-blocks", type=int, default=20, help="basic blocks in the profile file"
    )
    ap.add_argument(
        "--raw-in", default=None, help="skip the capture; use this raw file"
    )
    ap.add_argument(
        "--format", choices=("text", "json"), default="text", help="report format"
    )
    a = ap.parse_args(argv)
    if not a.out_dir:
        ap.error("--out-dir is required (or set $CORTRACE_OUT_DIR)")
    if not a.raw_in and not a.iface:
        ap.error("capture NIC not given: pass --iface or set $CORTRACE_IFACE")
    a.out_dir = os.path.abspath(a.out_dir)
    a.elf = os.path.abspath(a.elf)
    a.tag = a.tag or time.strftime("%Y%m%d-%H%M%S")
    return a


def prof_command(a, raw, syms, out):
    """cortrace-prof command line of a run."""
    cmd = [find_binary("CORTRACE_PROF", "cortrace-prof"), "--raw-in", raw]
    cmd += ["--elf", a.elf, "--syms", syms, "--out", out]
    cmd += ["--sysclk-hz", f"{a.sysclk_hz:g}", "--tsgen-hz", f"{a.tsgen_hz:g}"]
    cmd += ["--period-exception", str(a.period_exception)]
    cmd += [
        "--period-cycles",
        f"{a.period_cycles:g}",
        "--top-blocks",
        str(a.top_blocks),
    ]
    cmd += ["--trace-width", str(a.width or 4)]
    if a.phase:
        cmd += ["--phase", a.phase]
    return cmd


def write_symbols(a, path):
    """`nm -n` of the ELF into `path`."""
    try:
        with open(path, "w", encoding="utf-8") as f:
            subprocess.run([a.nm, "-n", a.elf], stdout=f, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ProfError(f"cannot get the symbols with {a.nm}: {exc}") from exc


def run_prof(a, raw, out):
    syms = os.path.join(tempfile.gettempdir(), f"cortrace_prof_{os.getpid()}.nm")
    try:
        write_symbols(a, syms)
        cmd = prof_command(a, raw, syms, out)
        log("$ " + " ".join(cmd))
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        except OSError as exc:
            raise ProfError(f"cannot run {cmd[0]}: {exc}") from exc
    finally:
        if os.path.exists(syms):
            os.remove(syms)
    for line in proc.stderr.splitlines():
        log("  " + line.removeprefix("[cortrace-prof] ").strip())
    if proc.returncode != 0 or not os.path.isfile(out):
        raise ProfError("cortrace-prof failed")


def cmd_run(argv):
    a = parse_run_args(argv)
    if a.raw_in:
        raw = os.path.abspath(a.raw_in)
        if not os.path.isfile(raw):
            raise ProfError(f"input not found: {raw}")
        os.makedirs(a.out_dir, exist_ok=True)
    else:
        runstore.check_duration(a.secs, a.allow_long)
        runstore.require_space(
            a.out_dir,
            int(a.secs * runstore.GRAB_BYTES_PER_SEC * runstore.HEADROOM),
            f"a {a.secs:g} s capture",
        )
        os.makedirs(a.out_dir, exist_ok=True)
        raw = os.path.join(a.out_dir, f"raw_{a.tag}.bin")
        # a lossy capture is useless here: the cycle clock needs every count
        capture.grab(a, raw)
    out = os.path.join(a.out_dir, f"prof_{a.tag}.json")
    run_prof(a, raw, out)
    log(f"profile -> {out}")
    prof = load(out)
    if a.format == "json":
        sys.stdout.write(json.dumps(prof, indent=1) + "\n")
    else:
        sys.stdout.write(text(prof, a.top))
    return 0


# ---- report / diff -------------------------------------------------------------------------
def cmd_report(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace profile report", description="report a profile file"
    )
    ap.add_argument("profile", help="prof_<tag>.json")
    ap.add_argument("--top", type=int, default=25, help="functions listed")
    ap.add_argument("--format", choices=("text", "json"), default="text")
    ap.add_argument(
        "-o", "--output", default=None, help="write the report here (default: stdout)"
    )
    a = ap.parse_args(argv)
    prof = load(a.profile)
    body = (
        json.dumps(prof, indent=1) + "\n" if a.format == "json" else text(prof, a.top)
    )
    if a.output:
        with open(a.output, "w", encoding="utf-8") as f:
            f.write(body)
        log(f"wrote {a.format} report -> {a.output}")
    else:
        sys.stdout.write(body)
    return 0


def cmd_diff(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace profile diff",
        description="compare two profile files (A -> B)",
    )
    ap.add_argument("a", help="profile file A (the baseline)")
    ap.add_argument("b", help="profile file B")
    ap.add_argument("--top", type=int, default=25, help="functions listed")
    a = ap.parse_args(argv)
    sys.stdout.write(diff(load(a.a), load(a.b), a.top))
    return 0


COMMANDS = {"run": cmd_run, "report": cmd_report, "diff": cmd_diff}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help") or argv[0] not in COMMANDS:
        print(__doc__.strip())
        print("\nusage: cortrace profile {run,report,diff} [options]  (--help each)")
        if argv and argv[0] not in ("-h", "--help"):
            print(f"\ncortrace profile: unknown command {argv[0]!r}", file=sys.stderr)
            return ERROR
        return 0 if argv else ERROR
    try:
        return COMMANDS[argv[0]](argv[1:])
    except ProfError as exc:
        print(f"cortrace profile: {exc}", file=sys.stderr)
        return ERROR
