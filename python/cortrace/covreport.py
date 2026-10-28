"""Coverage reports from a covmodel.Summary: text (laid out like Ozone's Code
Profile report), JSON, lcov .info and Cobertura XML.

Counting rules, shared by every format:
  - an instruction is covered when it was seen (in `confirm` distinct trace
    segments or more);
  - a source line is covered when at least one of its instructions is (lcov's
    DA > 0); the JSON also says whether it is fully covered or partial;
  - a branch is one outcome (taken or not taken) of a conditional branch
    instruction, so every conditional branch has two;
  - a function is covered when any of its instructions is.
"""

import os
import time
import xml.etree.ElementTree as ET

from . import covmodel


def display_path(path, src_root):
    """`path` relative to src_root when it is inside it."""
    if src_root and os.path.isabs(path):
        rel = os.path.relpath(path, src_root)
        if not rel.startswith(".."):
            return rel
    return path


def common_root(paths):
    """Deepest directory shared by the absolute source paths ('' if none)."""
    dirs = [os.path.dirname(p) for p in paths if os.path.isabs(p)]
    if not dirs:
        return ""
    root = os.path.commonpath(dirs)
    return "" if root == os.sep else root


# ---- text -------------------------------------------------------------------
def _cell(c):
    pct = c.pct()
    if pct is None:
        return f"{c.covered:>6} / {c.total:<6}       "
    return f"{c.covered:>6} / {c.total:<6} {pct:5.1f}%"


def health_line(summary):
    """One-line trace health of a summary."""
    h = summary.health
    quarantined = h.get("quarantined", 0)
    words = [
        f"{covmodel.losses(h)} trace losses",
        f"{covmodel.mismatches(h)} cross-check mismatches",
        f"{quarantined} of {h.get('segments', 0)} segments quarantined",
        f"confirm={summary.confirm}",
    ]
    return "Trace health: " + ", ".join(words) + f"  -> {summary.status}"


def text(summary, src_root=None, functions=True):
    """Ozone-style coverage table."""
    cov = summary.cov
    out = ["Cortrace Code Coverage Report"]
    runs = cov.get("runs", [])
    secs = sum(r.get("secs") or 0 for r in runs)
    first = runs[0] if runs else {}
    out.append(
        f"ELF: {os.path.basename(cov['elf']['path'])}  sha256: "
        f"{cov['elf']['sha256'][:12]}  runs: {len(runs)}"
        + (f"  captured: {first['captured']}" if first.get("captured") else "")
        + (f"  duration: {secs:g} s" if secs else "")
    )
    out.append(health_line(summary))
    if not summary.complete:
        out.append(
            "  (trace was lost or failed a cross-check: code not seen is "
            '"not observed", it may still have run)'
        )
    files = summary.sorted_files()
    if not src_root:
        src_root = common_root(f.path for f in files)
    if src_root:
        out.append(f"Sources: {src_root}")
    names = [display_path(f.path, src_root) for f in files]
    width = max(
        [len("Module/Function"), len("  Total")] + [min(len(n), 48) for n in names]
    )
    if functions:
        fn_names = [fn.name for f in files for fn in f.functions]
        width = max([width] + [min(len(n) + 2, 48) for n in fn_names])
    col = len(_cell(covmodel.Counter(1, 1)))
    head = (
        f"{'Module/Function':<{width}} | {'Source Lines':<{col}} | "
        f"{'Instructions':<{col}} | {'Branches':<{col}}"
    )
    rule = "-" * width + "-+-" + "-+-".join(["-" * col] * 3)
    out += ["", head, rule]
    for fc, name in zip(files, names):
        if len(name) > width:
            name = "..." + name[-(width - 3) :]
        out.append(
            f"{name:<{width}} | {_cell(fc.line_counter())} | "
            f"{_cell(fc.insn)} | {_cell(fc.branches)}"
        )
        if not functions:
            continue
        for fn in sorted(fc.functions, key=lambda f: f.addr):
            label = ("  " + fn.name)[:width]
            out.append(
                f"{label:<{width}} | {_cell(fn.line_counter())} | "
                f"{_cell(fn.insn)} | {_cell(fn.branches)}"
            )
    totals = summary.totals()
    out += [
        rule,
        f"{'  Total':<{width}} | {_cell(totals['lines'])} | "
        f"{_cell(totals['insns'])} | {_cell(totals['branches'])}",
        "",
        f"Functions: {_cell(totals['functions']).strip()}",
    ]
    return "\n".join(out) + "\n"


# ---- JSON -------------------------------------------------------------------
def to_json(summary, src_root=None):
    """Machine-readable report: totals, per file, per function and per line."""
    cov = summary.cov
    files = []
    for fc in summary.sorted_files():
        lines = []
        for number in sorted(fc.lines):
            ln = fc.lines[number]
            lines.append(
                {
                    "line": number,
                    "status": ln.status(summary.complete),
                    "hits": ln.hits,
                    "insn": ln.insn.pair(),
                    "br": ln.br().pair(),
                }
            )
        functions = [
            {
                "name": fn.name,
                "addr": fn.addr,
                "line": fn.line,
                "hits": fn.hits,
                "lines": fn.line_counter().pair(),
                "insn": fn.insn.pair(),
                "br": fn.branches.pair(),
            }
            for fn in sorted(fc.functions, key=lambda f: f.addr)
        ]
        files.append(
            {
                "path": display_path(fc.path, src_root),
                "lines": fc.line_counter().pair(),
                "insns": fc.insn.pair(),
                "branches": fc.branches.pair(),
                "functions": fc.function_counter().pair(),
                "line_detail": lines,
                "function_detail": functions,
            }
        )
    return {
        "format": "cortrace-cov-report",
        "version": covmodel.VERSION,
        "elf": cov["elf"],
        "runs": cov.get("runs", []),
        "status": summary.status,
        "confirm": summary.confirm,
        "health": summary.health,
        "totals": {k: v.pair() for k, v in summary.totals().items()},
        "files": files,
    }


# ---- lcov -------------------------------------------------------------------
def lcov(summary, src_root=None, test_name=""):
    """lcov tracefile (genhtml, VS Code Coverage Gutters, coverage services)."""
    out = []
    for fc in summary.sorted_files():
        if fc.path == covmodel.UNKNOWN_FILE:
            continue
        out.append(f"TN:{test_name}")
        out.append(f"SF:{display_path(fc.path, src_root)}")
        fns = sorted(fc.functions, key=lambda f: f.addr)
        for fn in fns:
            out.append(f"FN:{fn.line},{fn.name}")
        for fn in fns:
            count = fn.hits or (1 if fn.insn.covered else 0)
            out.append(f"FNDA:{count},{fn.name}")
        out.append(f"FNF:{len(fns)}")
        out.append(f"FNH:{sum(1 for fn in fns if fn.insn.covered)}")
        brf = brh = 0
        for number in sorted(fc.lines):
            for block, (taken, not_taken, executed) in enumerate(
                fc.lines[number].branches
            ):
                for branch, count in ((0, taken), (1, not_taken)):
                    shown = str(count) if executed else "-"
                    out.append(f"BRDA:{number},{block},{branch},{shown}")
                    brf += 1
                    brh += count > 0
        out.append(f"BRF:{brf}")
        out.append(f"BRH:{brh}")
        for number in sorted(fc.lines):
            out.append(f"DA:{number},{fc.lines[number].hits}")
        lc = fc.line_counter()
        out.append(f"LF:{lc.total}")
        out.append(f"LH:{lc.covered}")
        out.append("end_of_record")
    return "\n".join(out) + ("\n" if out else "")


# ---- Cobertura --------------------------------------------------------------
def _rate(c):
    return f"{(c.covered / c.total) if c.total else 1.0:.4f}"


def _line_element(parent, number, ln):
    attrs = {"number": str(number), "hits": str(ln.hits)}
    br = ln.br()
    if br.total:
        pct = int(round(100.0 * br.covered / br.total))
        attrs["branch"] = "true"
        attrs["condition-coverage"] = f"{pct}% ({br.covered}/{br.total})"
    else:
        attrs["branch"] = "false"
    ET.SubElement(parent, "line", attrs)


def cobertura(summary, src_root=None):
    """Cobertura XML (CI coverage widgets)."""
    totals = summary.totals()
    root = ET.Element(
        "coverage",
        {
            "line-rate": _rate(totals["lines"]),
            "branch-rate": _rate(totals["branches"]),
            "lines-covered": str(totals["lines"].covered),
            "lines-valid": str(totals["lines"].total),
            "branches-covered": str(totals["branches"].covered),
            "branches-valid": str(totals["branches"].total),
            "complexity": "0",
            "version": "cortrace",
            "timestamp": str(int(time.time())),
        },
    )
    sources = ET.SubElement(root, "sources")
    ET.SubElement(sources, "source").text = src_root or "/"
    packages = ET.SubElement(root, "packages")
    by_dir = {}
    for fc in summary.sorted_files():
        if fc.path == covmodel.UNKNOWN_FILE:
            continue
        name = display_path(fc.path, src_root)
        by_dir.setdefault(os.path.dirname(name), []).append((name, fc))
    for directory in sorted(by_dir):
        line_c = covmodel.Counter()
        br_c = covmodel.Counter()
        for _, fc in by_dir[directory]:
            for c, acc in ((fc.line_counter(), line_c), (fc.branches, br_c)):
                acc.covered += c.covered
                acc.total += c.total
        pkg = ET.SubElement(
            packages,
            "package",
            {
                "name": directory.strip("/").replace("/", ".") or ".",
                "line-rate": _rate(line_c),
                "branch-rate": _rate(br_c),
                "complexity": "0",
            },
        )
        classes = ET.SubElement(pkg, "classes")
        for name, fc in by_dir[directory]:
            cls = ET.SubElement(
                classes,
                "class",
                {
                    "name": os.path.basename(name),
                    "filename": name.lstrip("/") if not src_root else name,
                    "line-rate": _rate(fc.line_counter()),
                    "branch-rate": _rate(fc.branches),
                    "complexity": "0",
                },
            )
            methods = ET.SubElement(cls, "methods")
            for fn in sorted(fc.functions, key=lambda f: f.addr):
                method = ET.SubElement(
                    methods,
                    "method",
                    {
                        "name": fn.name,
                        "signature": "",
                        "line-rate": _rate(fn.line_counter()),
                        "branch-rate": _rate(fn.branches),
                    },
                )
                mlines = ET.SubElement(method, "lines")
                own = sorted(
                    line
                    for (fidx, line) in fn.lines
                    if summary.cov["files"][fidx] == fc.path and line in fc.lines
                )
                for number in own:
                    _line_element(mlines, number, fc.lines[number])
            lines = ET.SubElement(cls, "lines")
            for number in sorted(fc.lines):
                _line_element(lines, number, fc.lines[number])
    ET.indent(root)
    return '<?xml version="1.0" ?>\n' + ET.tostring(root, encoding="unicode") + "\n"


FORMATS = ("text", "json", "lcov", "cobertura")
