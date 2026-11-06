"""stats -- call-graph, per-function and per-thread statistics, and flame graphs.

  report  print a stats file (written by `cortrace capture --stats`, or by
          `cortrace-decode --stats-out`) as text
  flame   turn a folded-stack file into a self-contained SVG flame graph

The numbers are CPU cycles from the ETM cycle counts, so the trace has to be
captured with the timing profile (`cortrace arm --profile timing`). A call is
counted only when the trace is intact between its start and its end; calls that
span lost trace are counted apart and add no time (see docs/12-call-stats.md).
"""

import argparse
import colorsys
import json
import sys
import zlib
from dataclasses import dataclass, field
from typing import Optional
from xml.sax.saxutils import escape

FORMAT = "cortrace-callstats"
VERSION = 1
ERROR = 2


class StatsError(Exception):
    """A problem the user can fix (bad input)."""


def log(msg):
    print(f"[cortrace-stats] {msg}", file=sys.stderr)


# ---- stats file ---------------------------------------------------------------------------


def load(path):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        raise StatsError(f"cannot read {path}: {exc}") from exc
    if not isinstance(data, dict) or data.get("format") != FORMAT:
        raise StatsError(f"{path} is not a cortrace-callstats file")
    if data.get("version") != VERSION:
        raise StatsError(
            f"{path}: stats version {data.get('version')}, expected {VERSION}"
        )
    return data


# ---- text ---------------------------------------------------------------------------------


def num(n):
    return f"{int(n):,}"


def pct(part, whole):
    return f"{part / whole * 100:5.1f}%" if whole else "    -"


@dataclass
class Units:
    """Cycles, or microseconds when the file knows the CPU clock."""

    name: str = "cycles"
    scale: Optional[float] = None  # microseconds per cycle

    def fmt(self, cycles):
        if self.scale is None:
            return num(cycles)
        return f"{cycles * self.scale:,.2f}"


def units_for(stats, unit):
    if unit != "us":
        return Units()
    hz = stats.get("sysclk_hz")
    if not hz:
        raise StatsError("--unit us needs the CPU clock: capture with --sysclk-hz")
    return Units("us", 1e6 / hz)


def table(headers, rows, left=1):
    """Right-aligned columns; the first `left` columns are left-aligned."""
    cols = list(zip(*([headers] + rows))) if rows else [[h] for h in headers]
    widths = [max(len(str(c)) for c in col) for col in cols]
    out = []
    for r in [headers] + rows:
        cells = [
            str(c).ljust(w) if i < left else str(c).rjust(w)
            for i, (c, w) in enumerate(zip(r, widths))
        ]
        out.append("  " + "  ".join(cells))
    return "\n".join(out)


def header(stats, units):
    h = stats["health"]
    total = h["credited"] + h["dropped"]
    lines = [f"cortrace stats: {stats.get('elf') or '-'}"]
    when = f"{units.fmt(h['credited'])} {units.name}"
    if units.scale is None and stats.get("sysclk_hz"):
        when += f" = {h['credited'] / stats['sysclk_hz'] * 1e3:,.2f} ms at "
        when += f"{stats['sysclk_hz'] / 1e6:g} MHz"
    lines.append(f"observed   : {when}")
    if h["losses"] or h["dropped"] or h["crossed_frames"]:
        lines.append(
            f"lost trace : {num(h['losses'])} events; "
            f"{units.fmt(h['dropped'])} {units.name} dropped ({pct(h['dropped'], total).strip()} "
            f"of the trace); {num(h['crossed_frames'])} calls spanned it and are not counted"
        )
    else:
        lines.append("lost trace : none")
    lines.append(
        f"open at end: {num(h['truncated_frames'])} calls (not counted: their end is unknown)"
    )
    return "\n".join(lines)


def text_threads(stats, units):
    rows = []
    credited = stats["health"]["credited"]
    for t in stats["threads"]:
        rows.append(
            [
                t["name"],
                units.fmt(t["run"]),
                pct(t["run"], credited),
                units.fmt(t["isr"]),
                num(t["isr_entries"]),
                num(t["intervals"]),
                units.fmt(t["longest"]),
                num(t["lossy_intervals"]),
            ]
        )
    head = [
        "thread",
        f"run ({units.name})",
        "run%",
        f"isr ({units.name})",
        "isr entries",
        "switch-ins",
        f"longest ({units.name})",
        "lossy",
    ]
    return (
        "threads (run = the thread's own code; isr = interrupts that preempted it; "
        "longest = longest switch-in to switch-out with intact trace)\n"
        + table(head, rows)
    )


SORTS = {
    "self": lambda f: (-f["self"], f["name"]),
    "incl": lambda f: (-f["incl"], f["name"]),
    "calls": lambda f: (-f["calls"], f["name"]),
}


def text_functions(stats, units, top, sort):
    credited = stats["health"]["credited"]
    funcs = sorted(stats["functions"], key=SORTS[sort])
    rows = []
    for f in funcs[:top]:
        rows.append(
            [
                f["name"],
                num(f["calls"]),
                units.fmt(f["self"]),
                pct(f["self"], credited),
                units.fmt(f["incl"]),
                pct(f["incl"], credited),
                units.fmt(f["p50"]),
                units.fmt(f["p99"]),
                units.fmt(f["max"]),
            ]
        )
    head = [
        "function",
        "calls",
        f"self ({units.name})",
        "self%",
        f"incl ({units.name})",
        "incl%",
        "p50",
        "p99",
        "max",
    ]
    extra = f" of {len(funcs)}" if len(funcs) > top else ""
    return (
        f"functions by {sort} (top {min(top, len(funcs))}{extra}; p50/p99/max are per call, "
        f"inclusive, in {units.name})\n" + table(head, rows)
    )


def text_edges(stats, units, top):
    edges = sorted(stats["edges"], key=lambda e: (-e["incl"], e["caller"], e["callee"]))
    rows = [
        [e["caller"], e["callee"], num(e["calls"]), units.fmt(e["incl"])]
        for e in edges[:top]
    ]
    head = ["caller", "callee", "calls", f"incl ({units.name})"]
    return f"call edges by inclusive time (top {min(top, len(edges))})\n" + table(
        head, rows, left=2
    )


def find_function(stats, name):
    exact = [f for f in stats["functions"] if f["name"] == name]
    if exact:
        return exact[0]
    part = [f for f in stats["functions"] if name in f["name"]]
    if len(part) == 1:
        return part[0]
    if not part:
        raise StatsError(f"no function named {name!r} in the stats")
    names = ", ".join(f["name"] for f in part[:8])
    raise StatsError(f"{name!r} matches {len(part)} functions: {names} ...")


def text_function(stats, units, name):
    f = find_function(stats, name)
    credited = stats["health"]["credited"]
    lines = [
        f"{f['name']}: {num(f['calls'])} calls, self {units.fmt(f['self'])} "
        f"({pct(f['self'], credited).strip()}), inclusive {units.fmt(f['incl'])} "
        f"({pct(f['incl'], credited).strip()}) {units.name}",
        f"  per call: min {units.fmt(f['min'])}  p50 {units.fmt(f['p50'])}  "
        f"p90 {units.fmt(f['p90'])}  p99 {units.fmt(f['p99'])}  max {units.fmt(f['max'])}",
    ]
    if f["crossed"] or f["truncated"]:
        lines.append(
            f"  not counted: {num(f['crossed'])} calls spanned lost trace, "
            f"{num(f['truncated'])} were still open at the end"
        )
    callers = sorted(
        (e for e in stats["edges"] if e["callee"] == f["name"]),
        key=lambda e: -e["incl"],
    )
    callees = sorted(
        (e for e in stats["edges"] if e["caller"] == f["name"]),
        key=lambda e: -e["incl"],
    )
    head = ["calls", f"incl ({units.name})", "share"]
    rows = [
        [
            e["caller"],
            num(e["calls"]),
            units.fmt(e["incl"]),
            pct(e["incl"], f["incl"]),
        ]
        for e in callers
    ]
    lines += ["", "called from:", table(["caller"] + head, rows)]
    rows = [
        [
            e["callee"],
            num(e["calls"]),
            units.fmt(e["incl"]),
            pct(e["incl"], f["incl"]),
        ]
        for e in callees
    ]
    lines += ["", "calls:", table(["callee"] + head, rows)]
    return "\n".join(lines)


def text(stats, *, top=20, sort="self", unit="cycles", edges=0, func=None):
    units = units_for(stats, unit)
    parts = [header(stats, units)]
    if func:
        parts.append(text_function(stats, units, func))
        return "\n\n".join(parts) + "\n"
    if stats["threads"]:
        parts.append(text_threads(stats, units))
    parts.append(text_functions(stats, units, top, sort))
    if edges:
        parts.append(text_edges(stats, units, edges))
    return "\n\n".join(parts) + "\n"


# ---- flame graph --------------------------------------------------------------------------


@dataclass
class Node:
    name: str
    total: int = 0
    self_: int = 0
    children: dict = field(default_factory=dict)


def parse_folded(lines):
    """Folded stacks ("a;b;c N") -> root node. A malformed line is an error."""
    root = Node("all")
    for n, line in enumerate(lines, 1):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        stack, _, count = line.rpartition(" ")
        if not stack or not count.isdigit():
            raise StatsError(f"folded line {n} is not 'stack count': {line[:60]!r}")
        value = int(count)
        node = root
        node.total += value
        for frame in stack.split(";"):
            node = node.children.setdefault(frame, Node(frame))
            node.total += value
        node.self_ += value
    return root


def depth(root):
    """Levels of the tree. Iterative: recursive code paths make stacks hundreds
    of frames deep, deeper than a Python recursion allows."""
    deepest = 0
    pending = [(root, 1)]
    while pending:
        node, level = pending.pop()
        deepest = max(deepest, level)
        pending.extend((c, level + 1) for c in node.children.values())
    return deepest


def color(name):
    """A warm, stable colour per function name."""
    h = zlib.crc32(name.encode("utf-8"))
    hue = 0.02 + (h % 1000) / 1000 * 0.12  # red to yellow
    sat = 0.55 + ((h >> 10) % 100) / 100 * 0.35
    r, g, b = colorsys.hsv_to_rgb(hue, sat, 0.95)
    return f"#{int(r * 255):02x}{int(g * 255):02x}{int(b * 255):02x}"


def frame_svg(node, box, root_total, label):
    """The SVG of one frame: a tooltip, a box and, if it fits, the name.
    `box` is (x, y, width, row height, is_root)."""
    x, y, w, row, is_root = box
    tip = f"{node.name}\n{label(node.total)} ({node.total / root_total * 100:.2f}%)"
    if node.self_:
        tip += f"\nself {label(node.self_)}"
    fill = "#c8c8c8" if is_root else color(node.name)
    out = [
        f'<g><title>{escape(tip)}</title><rect x="{x:.2f}" y="{y}" width="{w:.2f}" '
        f'height="{row - 1}" fill="{fill}" rx="2"/>'
    ]
    chars = int((w - 6) / 6.6)
    if chars >= 3:
        shown = node.name if len(node.name) <= chars else node.name[: chars - 2] + ".."
        out.append(
            f'<text x="{x + 3:.2f}" y="{y + 12}" font-size="11" '
            f'font-family="monospace">{escape(shown)}</text>'
        )
    out.append("</g>")
    return out


def flame_svg(root, *, title="cortrace", width=1200, min_pct=0.05, hz=None):
    """An SVG flame graph of a folded-stack tree (cycles; us too with `hz`)."""
    if root.total <= 0:
        raise StatsError("the folded file has no cycles")
    row, top, pad = 17, 34, 8
    height = top + depth(root) * row + pad
    scale = (width - 2 * pad) / root.total
    min_value = root.total * min_pct / 100

    def label(value):
        s = f"{value:,} cycles"
        if hz:
            s += f" = {value / hz * 1e6:,.2f} us"
        return s

    rects = []
    pending = [(root, pad, 0)]
    while pending:
        node, x, level = pending.pop()
        box = (x, height - pad - (level + 1) * row, node.total * scale, row, level == 0)
        rects += frame_svg(node, box, root.total, label)
        # children side by side in name order; the stack pops them in reverse
        cx = x
        kids = []
        for child in sorted(node.children.values(), key=lambda c: c.name):
            if child.total >= min_value:
                kids.append((child, cx, level + 1))
            cx += child.total * scale
        pending.extend(reversed(kids))
    head = (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">\n'
        f'<rect width="100%" height="100%" fill="#fafafa"/>\n'
        f'<text x="{width / 2}" y="20" text-anchor="middle" font-size="15" '
        f'font-family="sans-serif">{escape(title)}: {escape(label(root.total))}, '
        f"self time per stack (hover for details)</text>\n"
    )
    return head + "\n".join(rects) + "\n</svg>\n"


# ---- commands -----------------------------------------------------------------------------


def cmd_report(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace stats report", description="report a stats file"
    )
    ap.add_argument("stats", help="stats_<tag>.json")
    ap.add_argument("--top", type=int, default=20, help="functions listed")
    ap.add_argument(
        "--sort", choices=sorted(SORTS), default="self", help="function order"
    )
    ap.add_argument(
        "--unit",
        choices=("cycles", "us"),
        default="cycles",
        help="us needs the CPU clock in the file (--sysclk-hz at capture)",
    )
    ap.add_argument(
        "--edges", type=int, default=0, metavar="N", help="also list N call edges"
    )
    ap.add_argument(
        "--func",
        default=None,
        metavar="NAME",
        help="callers and callees of one function (a unique substring is enough)",
    )
    ap.add_argument("--format", choices=("text", "json"), default="text")
    ap.add_argument("-o", "--output", default=None, help="write here (default: stdout)")
    a = ap.parse_args(argv)
    stats = load(a.stats)
    if a.format == "json":
        body = json.dumps(stats, indent=1) + "\n"
    else:
        body = text(
            stats, top=a.top, sort=a.sort, unit=a.unit, edges=a.edges, func=a.func
        )
    if a.output:
        with open(a.output, "w", encoding="utf-8") as f:
            f.write(body)
        log(f"wrote {a.format} report -> {a.output}")
    else:
        sys.stdout.write(body)
    return 0


def cmd_flame(argv):
    ap = argparse.ArgumentParser(
        prog="cortrace stats flame",
        description="folded stacks -> SVG flame graph (open it in a browser). "
        "The folded file also loads in speedscope and flamegraph.pl.",
    )
    ap.add_argument("folded", help="folded_<tag>.txt")
    ap.add_argument("-o", "--output", required=True, help="SVG file to write")
    ap.add_argument("--title", default="cortrace")
    ap.add_argument("--width", type=int, default=1200)
    ap.add_argument(
        "--min-pct",
        type=float,
        default=0.05,
        help="leave out frames below this share of the total (default %(default)s)",
    )
    ap.add_argument(
        "--sysclk-hz", type=float, default=None, help="CPU clock: tooltips show us too"
    )
    a = ap.parse_args(argv)
    try:
        with open(a.folded, encoding="utf-8") as f:
            root = parse_folded(f)
    except OSError as exc:
        raise StatsError(f"cannot read {a.folded}: {exc}") from exc
    svg = flame_svg(
        root, title=a.title, width=a.width, min_pct=a.min_pct, hz=a.sysclk_hz
    )
    with open(a.output, "w", encoding="utf-8") as f:
        f.write(svg)
    log(f"wrote flame graph -> {a.output}")
    return 0


COMMANDS = {"report": cmd_report, "flame": cmd_flame}


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help") or argv[0] not in COMMANDS:
        print(__doc__.strip())
        print("\nusage: cortrace stats {report,flame} [options]  (--help each)")
        if argv and argv[0] not in ("-h", "--help"):
            print(f"\ncortrace stats: unknown command {argv[0]!r}", file=sys.stderr)
            return ERROR
        return 0 if argv else ERROR
    try:
        return COMMANDS[argv[0]](argv[1:])
    except StatsError as exc:
        print(f"cortrace stats: {exc}", file=sys.stderr)
        return ERROR
