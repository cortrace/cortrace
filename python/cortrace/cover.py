"""cortrace cover -- code coverage from the ETM trace, without instrumentation.

  run     capture (or take a raw file), decode with --cov-out, attach the
          source lines and functions of the ELF, write cov_<tag>.json and
          print a report
  report  report a coverage file as text, JSON, lcov or Cobertura
  merge   add up several runs of the same ELF
  diff    compare two coverage files (e.g. a baseline and a new build)

Coverage is "observed in clean trace": the decoder counts only the trace
segments between A-syncs that decoded without a loss and agreed with the ELF's
code. When the session lost trace, code that was not seen is reported as
"not observed" rather than "uncovered".

Exit codes: 0 fine, 1 a --fail-under / --require-lossless / --fail-on-regression
check failed, 2 usage or input error.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

from . import capture, covmodel, covreport, runstore
from ._paths import find_binary

GATE_FAILED = 1
ERROR = 2


def log(msg):
    print(f"[cortrace-cover] {msg}", file=sys.stderr)


# ---- report options ---------------------------------------------------------
def add_selection_options(ap):
    ap.add_argument(
        "--confirm",
        type=int,
        default=1,
        metavar="N",
        help="count an instruction or branch outcome only when it was seen in N "
        "distinct trace segments (a single corrupted segment cannot then fake "
        "coverage; default: %(default)s)",
    )
    ap.add_argument(
        "--include",
        action="append",
        default=[],
        metavar="GLOB",
        help="only source files matching GLOB (repeatable), e.g. '*/app/*'",
    )
    ap.add_argument(
        "--exclude",
        action="append",
        default=[],
        metavar="GLOB",
        help="leave out source files matching GLOB (repeatable), e.g. '*/newlib*'",
    )
    ap.add_argument(
        "--src-root",
        default=None,
        metavar="DIR",
        help="show source paths relative to DIR when they are inside it",
    )


def add_report_options(ap):
    add_selection_options(ap)
    ap.add_argument(
        "--format", choices=covreport.FORMATS, default="text", help="report format"
    )
    ap.add_argument(
        "-o", "--output", default=None, help="write the report here (default: stdout)"
    )
    ap.add_argument(
        "--no-functions",
        action="store_true",
        help="text report: one row per file, no function rows",
    )
    ap.add_argument(
        "--fail-under",
        default=None,
        metavar="SPEC",
        help="exit 1 when coverage is below a threshold, e.g. "
        "lines=80,branches=60 (metrics: lines, insns, branches, functions)",
    )
    ap.add_argument(
        "--require-lossless",
        action="store_true",
        help="exit 1 when the session lost trace or failed a cross-check",
    )


def render(summary, a):
    if a.format == "json":
        import json

        return json.dumps(covreport.to_json(summary, a.src_root), indent=1) + "\n"
    if a.format == "lcov":
        tag = summary.cov["runs"][0].get("tag", "") if summary.cov["runs"] else ""
        return covreport.lcov(summary, a.src_root, test_name=tag)
    if a.format == "cobertura":
        return covreport.cobertura(summary, a.src_root)
    return covreport.text(summary, a.src_root, functions=not a.no_functions)


def summarize(cov, a):
    return covmodel.summarize(cov, a.confirm, a.include, a.exclude)


def emit(cov, a):
    """Write the report of `cov` and apply the CI gates; returns the exit code."""
    summary = summarize(cov, a)
    body = render(summary, a)
    if a.output:
        with open(a.output, "w", encoding="utf-8") as f:
            f.write(body)
        log(f"wrote {a.format} report -> {a.output}")
    else:
        sys.stdout.write(body)
    code = 0
    if a.require_lossless and not summary.complete:
        log(
            "FAIL --require-lossless: "
            + covreport.health_line(summary).removeprefix("Trace health: ")
        )
        code = GATE_FAILED
    for key, pct, want in covmodel.below_thresholds(
        summary, covmodel.parse_thresholds(a.fail_under)
    ):
        log(f"FAIL --fail-under: {key} {pct:.1f}% < {want:g}%")
        code = GATE_FAILED
    return code


# ---- run --------------------------------------------------------------------
def parse_run_args(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace cover run",
        description="capture -> decode -> coverage file and report",
    )
    ap.add_argument(
        "--out-dir",
        default=os.environ.get("CORTRACE_OUT_DIR"),
        help="directory for the raw capture and cov_<tag>.json (required; "
        "default: $CORTRACE_OUT_DIR)",
    )
    ap.add_argument("--elf", required=True, help="firmware ELF (with -g for lines)")
    ap.add_argument(
        "--iface", default=os.environ.get("CORTRACE_IFACE"), help="capture NIC"
    )
    ap.add_argument("--secs", type=float, default=1.0, help="capture seconds")
    ap.add_argument(
        "--allow-long",
        action="store_true",
        help=f"allow captures longer than {runstore.MAX_SECS:g} s",
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
    ap.add_argument(
        "--cov-lag",
        type=int,
        default=8,
        metavar="N",
        help="trace segments held back before they count, so that a later loss "
        "can still drop them (default: %(default)s)",
    )
    src = ap.add_mutually_exclusive_group()
    src.add_argument(
        "--raw-in", default=None, help="skip the capture; use this raw file"
    )
    src.add_argument(
        "--etm-in",
        default=None,
        help="skip the capture; use this file of plain ETM bytes "
        "(cortrace-decode --dump-etm)",
    )
    add_report_options(ap)
    a = ap.parse_args(argv)
    if not a.out_dir:
        ap.error("--out-dir is required (or set $CORTRACE_OUT_DIR)")
    if not (a.raw_in or a.etm_in) and not a.iface:
        ap.error("capture NIC not given: pass --iface or set $CORTRACE_IFACE")
    if a.cov_lag < 0:
        ap.error("--cov-lag must be >= 0")
    a.out_dir = os.path.abspath(a.out_dir)
    a.elf = os.path.abspath(a.elf)
    a.tag = a.tag or time.strftime("%Y%m%d-%H%M%S")
    return a


def decode_command(a, trace, syms, objcov):
    """cortrace-decode command line of a coverage run."""
    cmd = [find_binary("CORTRACE_DECODE", "cortrace-decode"), trace, syms]
    cmd += ["--elf", a.elf]
    if not a.etm_in:
        cmd += ["--raw", "--trace-width", str(a.width or 4)]
        if a.phase:
            cmd += ["--phase", a.phase]
    cmd += ["--memory-limit-mb", "0", "--cov-out", objcov, "--cov-lag", str(a.cov_lag)]
    return cmd


DECODE_KEYS = (
    "stream health",
    "instructions hit",
    "segments ",
    "losses ",
    "static cross-check",
    "error",
)


def decode(a, trace, objcov):
    """Run cortrace-decode --cov-out on `trace`."""
    syms = os.path.join(tempfile.gettempdir(), f"cortrace_cover_{os.getpid()}.nm")
    try:
        with open(syms, "w", encoding="utf-8") as f:
            subprocess.run([a.nm, "-n", a.elf], stdout=f, check=True)
        cmd = decode_command(a, trace, syms, objcov)
        log("$ " + " ".join(cmd))
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    finally:
        if os.path.exists(syms):
            os.remove(syms)
    for line in (proc.stderr + proc.stdout).splitlines():
        if any(k in line for k in DECODE_KEYS):
            log("  " + line.strip())
    if proc.returncode != 0 or not os.path.isfile(objcov):
        raise covmodel.CovError("decode failed")


def run(a):
    """Capture (or take the given file), decode, attach source; returns the
    coverage dict and writes cov_<tag>.json."""
    given = a.raw_in or a.etm_in
    if given:
        trace = os.path.abspath(given)
        if not os.path.isfile(trace):
            raise covmodel.CovError(f"input not found: {trace}")
        os.makedirs(a.out_dir, exist_ok=True)
        secs = None
    else:
        runstore.check_duration(a.secs, a.allow_long)
        runstore.require_space(
            a.out_dir,
            runstore.estimate_capture_bytes(a.secs),
            f"a {a.secs:g} s capture",
        )
        os.makedirs(a.out_dir, exist_ok=True)
        trace = os.path.join(a.out_dir, f"raw_{a.tag}.bin")
        capture.grab(a, trace, allow_loss=True)
        secs = a.secs
    objcov = os.path.join(a.out_dir, f"objcov_{a.tag}.json")
    try:
        decode(a, trace, objcov)
        obj = covmodel.load_objcov(objcov)
    finally:
        if os.path.exists(objcov):
            os.remove(objcov)
    meta = {
        "tag": a.tag,
        "captured": time.strftime("%Y-%m-%d %H:%M:%S"),
        "secs": secs,
        "trace": os.path.basename(trace),
    }
    cov = covmodel.attach_source(obj, a.elf, meta)
    path = os.path.join(a.out_dir, f"cov_{a.tag}.json")
    covmodel.save(cov, path)
    log(f"coverage -> {path}")
    return cov


def cmd_run(argv):
    a = parse_run_args(argv)
    return emit(run(a), a)


# ---- report / merge / diff --------------------------------------------------
def cmd_report(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace cover report", description="report a coverage file"
    )
    ap.add_argument("cov", help="cov_<tag>.json from `cortrace cover run` or merge")
    add_report_options(ap)
    a = ap.parse_args(argv)
    return emit(covmodel.load(a.cov), a)


def cmd_merge(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace cover merge",
        description="add up coverage files of the same ELF",
    )
    ap.add_argument("covs", nargs="+", help="coverage files")
    ap.add_argument("-o", "--output", required=True, help="merged coverage file")
    a = ap.parse_args(argv)
    cov = covmodel.merge([covmodel.load(p) for p in a.covs])
    covmodel.save(cov, a.output)
    summary = covmodel.summarize(cov)
    log(f"merged {len(a.covs)} files ({len(cov['runs'])} runs) -> {a.output}")
    log(covreport.health_line(summary))
    return 0


def _line_states(summary):
    """{(path, line): covered} and {(path, function): covered}."""
    lines = {}
    funcs = {}
    for fc in summary.files.values():
        for number, ln in fc.lines.items():
            lines[(fc.path, number)] = ln.insn.covered > 0
        for fn in fc.functions:
            funcs[(fc.path, fn.name)] = fn.insn.covered > 0
    return lines, funcs


def _fmt(c):
    pct = c.pct()
    return f"{c.covered}/{c.total}" + (f" {pct:.1f}%" if pct is not None else "")


def _totals_table(base, new):
    tb, tn = base.totals(), new.totals()
    out = [f"{'':<14}{'base':>22}{'new':>22}{'delta':>10}"]
    for key, label in (
        ("lines", "Lines"),
        ("insns", "Instructions"),
        ("branches", "Branches"),
        ("functions", "Functions"),
    ):
        pb, pn = tb[key].pct(), tn[key].pct()
        delta = f"{pn - pb:+.1f}%" if pb is not None and pn is not None else ""
        out.append(f"{label:<14}{_fmt(tb[key]):>22}{_fmt(tn[key]):>22}{delta:>10}")
    return out


def diff_text(base, new, src_root=None, limit=50):
    """Text of `cortrace cover diff`; returns (text, regressions)."""
    out = ["Cortrace Coverage Diff"]
    for label, s in (("base", base), ("new ", new)):
        elf = s.cov["elf"]
        out.append(
            f"  {label}: {os.path.basename(elf['path'])} sha256 {elf['sha256'][:12]}"
            f"  runs {len(s.cov['runs'])}  {s.status}"
        )
    out += [""] + _totals_table(base, new)
    lines_b, funcs_b = _line_states(base)
    lines_n, funcs_n = _line_states(new)
    lost = sorted(k for k, hit in lines_b.items() if hit and not lines_n.get(k, False))
    gained = sorted(
        k for k, hit in lines_n.items() if hit and not lines_b.get(k, False)
    )
    lost_fn = sorted(
        k for k, hit in funcs_b.items() if hit and not funcs_n.get(k, False)
    )
    word = "no longer covered" if new.complete else "no longer observed"
    sections = (
        (
            f"Functions {word}",
            [f"{k[1]} ({covreport.display_path(k[0], src_root)})" for k in lost_fn],
        ),
        (
            f"Lines {word}",
            [f"{covreport.display_path(p, src_root)}:{n}" for p, n in lost],
        ),
        (
            "Lines newly covered",
            [f"{covreport.display_path(p, src_root)}:{n}" for p, n in gained],
        ),
    )
    for title, items in sections:
        out += ["", f"{title}: {len(items)}"] + [f"  {item}" for item in items[:limit]]
        if len(items) > limit:
            out.append(f"  ... {len(items) - limit} more")
    return "\n".join(out) + "\n", len(lost) + len(lost_fn)


def cmd_diff(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace cover diff",
        description="compare two coverage files by source line and function",
    )
    ap.add_argument("base", help="baseline coverage file")
    ap.add_argument("new", help="new coverage file")
    add_selection_options(ap)
    ap.add_argument(
        "--limit", type=int, default=50, help="lines listed per section (default 50)"
    )
    ap.add_argument(
        "--fail-on-regression",
        action="store_true",
        help="exit 1 when a line or function covered in base is not in new",
    )
    a = ap.parse_args(argv)
    base = summarize(covmodel.load(a.base), a)
    new = summarize(covmodel.load(a.new), a)
    body, regressions = diff_text(base, new, a.src_root, a.limit)
    sys.stdout.write(body)
    if a.fail_on_regression and regressions:
        log(f"FAIL --fail-on-regression: {regressions} lines/functions lost coverage")
        return GATE_FAILED
    return 0


COMMANDS = {
    "run": cmd_run,
    "report": cmd_report,
    "merge": cmd_merge,
    "diff": cmd_diff,
}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help") or argv[0] not in COMMANDS:
        print(__doc__.strip())
        print(
            "\nusage: cortrace cover {run,report,merge,diff} [options]  (--help each)"
        )
        if argv and argv[0] not in ("-h", "--help"):
            print(f"\ncortrace cover: unknown command {argv[0]!r}", file=sys.stderr)
            return ERROR
        return 0 if argv else ERROR
    try:
        return COMMANDS[argv[0]](argv[1:])
    except covmodel.CovError as exc:
        print(f"cortrace cover: {exc}", file=sys.stderr)
        return ERROR
