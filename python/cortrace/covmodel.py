"""Code coverage data: the cortrace coverage file and what it says per source line.

`cortrace-decode --cov-out` writes object-level coverage ("cortrace-objcov"):
every Thumb instruction of the ELF with its execution count, the outcomes of
every conditional branch and the targets of indirect branches, counted over the
clean trace segments only, plus the trace health. That file knows addresses,
not source.

`attach_source` turns it into the cortrace coverage file ("cortrace-cov"): the
same data plus the source line of every instruction (DWARF line table), the
functions (.symtab) and the ELF's sha256. The coverage file is self-contained:
report, merge and diff never need the ELF again.

`summarize` reads a coverage file the way a report does: per source line,
function and file, with `confirm` (an instruction or branch outcome counts only
when it was seen in at least that many distinct trace segments) and
include/exclude globs.

Semantics: coverage is "observed in clean trace". When the session lost trace
(or a segment failed a static cross-check), code that was never seen is "not
observed", not "uncovered": it may have run while the trace was not usable.
"""

import bisect
import fnmatch
import hashlib
import json
import os

OBJ_FORMAT = "cortrace-objcov"
COV_FORMAT = "cortrace-cov"
VERSION = 1

# insns[i][2] (flags) = kind | cond << 3 | waypoint << 4
KIND_MASK = 0x7
FLAG_COND = 0x8
FLAG_WAYPOINT = 0x10
KIND_NAMES = ("none", "direct", "call", "indirect_call", "indirect", "return")

# Health counters that mean trace was lost (the decoder had to resync).
LOSS_KEYS = ("lost_sync", "addr_nacc", "trace_on", "overflow")
# Health counters that mean the decoded path disagreed with the ELF.
MISMATCH_KEYS = (
    "outside_code",
    "count_mismatch",
    "kind_mismatch",
    "cond_mismatch",
    "flow_mismatch",
    "return_mismatch",
)
NO_FILE = -1
METRICS = ("lines", "insns", "branches", "functions")


class CovError(Exception):
    """A coverage file is missing, malformed or cannot be combined."""


def sha256_file(path):
    """Hex sha256 of a file."""
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _read_json(path, fmt):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except OSError as exc:
        raise CovError(f"{path}: {exc.strerror}") from exc
    except ValueError as exc:
        raise CovError(f"{path}: not JSON ({exc})") from exc
    if not isinstance(data, dict) or data.get("format") != fmt:
        raise CovError(f"{path}: not a {fmt} file")
    if data.get("version") != VERSION:
        raise CovError(
            f"{path}: {fmt} version {data.get('version')} (this tool reads {VERSION})"
        )
    return data


def load_objcov(path):
    """The object coverage written by `cortrace-decode --cov-out`."""
    return _read_json(path, OBJ_FORMAT)


def load(path):
    """A cortrace coverage file."""
    return _read_json(path, COV_FORMAT)


def save(cov, path):
    """Write a cortrace coverage file (one instruction per line, diff-friendly)."""
    head = {k: v for k, v in cov.items() if k not in ("insns", "branches")}
    tmp = f"{path}.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        text = json.dumps(head, sort_keys=True)
        f.write(text[:-1] + ',\n"insns": [')
        f.write(",\n".join(json.dumps(r) for r in cov["insns"]))
        f.write('],\n"branches": [')
        f.write(",\n".join(json.dumps(r) for r in cov["branches"]))
        f.write("]}\n")
    os.replace(tmp, path)


# ---- source attribution ----------------------------------------------------
def _decode(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


class _FileTable:  # pylint: disable=too-few-public-methods
    """Interns source paths so that instructions refer to them by index."""

    def __init__(self):
        self.paths = []
        self._index = {}

    def intern(self, path):
        idx = self._index.get(path)
        if idx is None:
            idx = self._index[path] = len(self.paths)
            self.paths.append(path)
        return idx


def _program_files(lineprog, comp_dir, files):
    """{DWARF file number: interned index} of one line program."""
    header = lineprog.header
    v5 = header["version"] >= 5
    dirs = [_decode(d) for d in header["include_directory"]]
    out = {}
    for pos, entry in enumerate(header["file_entry"]):
        name = _decode(entry.name)
        if v5:  # file and directory numbers are 0-based, dir 0 = comp dir
            number = pos
            d = entry.dir_index
            base = dirs[d] if d < len(dirs) else comp_dir
        else:  # 1-based files; dir 0 = comp dir, 1.. = include_directory
            number = pos + 1
            d = entry.dir_index
            base = dirs[d - 1] if 0 < d <= len(dirs) else comp_dir
        path = name if os.path.isabs(name) else os.path.join(base, name)
        if not os.path.isabs(path):
            path = os.path.join(comp_dir, path)
        out[number] = files.intern(os.path.normpath(path))
    return out


def line_rows(dwarf, files):
    """[(lo, hi, file index, line)] address ranges of the DWARF line table,
    sorted by lo. Rows with line 0 (compiler-generated code) are left out."""
    rows = []
    for cu in dwarf.iter_CUs():
        lineprog = dwarf.line_program_for_CU(cu)
        if lineprog is None:
            continue
        comp_dir = _decode(
            getattr(cu.get_top_DIE().attributes.get("DW_AT_comp_dir"), "value", b"")
        )
        numbers = _program_files(lineprog, comp_dir, files)
        prev = None
        for entry in lineprog.get_entries():
            state = entry.state
            if state is None:
                continue
            if prev is not None and state.address > prev.address and prev.line:
                fidx = numbers.get(prev.file)
                if fidx is not None:
                    rows.append((prev.address, state.address, fidx, prev.line))
            prev = None if state.end_sequence else state
    rows.sort()
    return rows


class _RowIndex:  # pylint: disable=too-few-public-methods
    """Address -> (file, line) over sorted, possibly overlapping rows."""

    BACKTRACK = 16  # rows of discarded (gc'd) functions can overlap at address 0

    def __init__(self, rows):
        self.rows = rows
        self.los = [r[0] for r in rows]

    def at(self, addr):
        i = bisect.bisect_right(self.los, addr) - 1
        for j in range(i, max(i - self.BACKTRACK, -1), -1):
            lo, hi, fidx, line = self.rows[j]
            if lo <= addr < hi:
                return fidx, line
        return NO_FILE, 0


BIND_RANK = {"STB_GLOBAL": 0, "STB_WEAK": 1, "STB_LOCAL": 2}


def function_symbols(elf):
    """[(lo, hi, name)] of the STT_FUNC symbols, one per address (a global
    name wins over weak aliases such as the default IRQ handlers), sorted."""
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None:
        return []
    best = {}
    for sym in symtab.iter_symbols():
        if sym["st_info"]["type"] != "STT_FUNC" or sym["st_shndx"] == "SHN_UNDEF":
            continue
        if not sym.name:
            continue
        lo = sym["st_value"] & ~1
        key = (BIND_RANK.get(sym["st_info"]["bind"], 3), sym.name)
        if lo not in best or key < best[lo][0]:
            best[lo] = (key, sym.name, sym["st_size"])
    los = sorted(best)
    out = []
    for i, lo in enumerate(los):
        _, name, size = best[lo]
        if size:
            hi = lo + size
        else:  # unknown size: up to the next function
            hi = los[i + 1] if i + 1 < len(los) else lo + 2
        out.append((lo, hi, name))
    return out


def attach_source(obj, elf_path, run=None):
    """Coverage file from object coverage `obj` and its ELF (DWARF + symbols)."""
    from elftools.elf.elffile import ELFFile

    files = _FileTable()
    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        rows = line_rows(elf.get_dwarf_info(), files) if elf.has_dwarf_info() else []
        funcs = function_symbols(elf)
    index = _RowIndex(rows)
    insns = []
    for addr, size, flags, hits, segs in obj["insns"]:
        fidx, line = index.at(addr)
        insns.append([addr, size, flags, hits, segs, fidx, line])
    functions = []
    for lo, hi, name in funcs:
        fidx, line = index.at(lo)
        functions.append([name, lo, hi, fidx, line])
    run = dict(run or {})
    run.setdefault("commit_lag", obj.get("commit_lag"))
    run["health"] = obj.get("health", {})
    return {
        "format": COV_FORMAT,
        "version": VERSION,
        "elf": {"path": os.path.abspath(elf_path), "sha256": sha256_file(elf_path)},
        "runs": [run],
        "health": dict(obj.get("health", {})),
        "files": files.paths,
        "functions": functions,
        "indirect": obj.get("indirect", []),
        "insns": insns,
        "branches": obj.get("branches", []),
    }


# ---- merge ------------------------------------------------------------------
def _run_key(run):
    return (run.get("tag"), run.get("captured"), run.get("trace"))


def merge(covs):
    """One coverage file from several runs of the same ELF: counts and health
    add up (the trace segments of different runs are distinct)."""
    if not covs:
        raise CovError("nothing to merge")
    first = covs[0]
    out = json.loads(json.dumps(first))
    for other in covs[1:]:
        if other["elf"]["sha256"] != first["elf"]["sha256"]:
            raise CovError(
                "cannot merge runs of different ELFs "
                f"({first['elf']['sha256'][:12]} vs {other['elf']['sha256'][:12]}); "
                "use `cortrace cover diff` to compare builds"
            )
        if len(other["insns"]) != len(out["insns"]) or len(other["branches"]) != len(
            out["branches"]
        ):
            raise CovError("coverage files of the same ELF disagree on its code")
        for mine, theirs in zip(out["insns"], other["insns"]):
            mine[3] += theirs[3]
            mine[4] += theirs[4]
        for mine, theirs in zip(out["branches"], other["branches"]):
            for k in range(1, len(mine)):
                mine[k] += theirs[k]
        seen = {_run_key(r) for r in out["runs"]}
        if any(_run_key(r) in seen for r in other["runs"]):
            raise CovError(
                "the same run is in more than one input (it would count twice "
                "and fake --confirm)"
            )
        for key, value in other["health"].items():
            out["health"][key] = out["health"].get(key, 0) + value
        out["runs"] += other["runs"]
        targets = {site: set(t) for site, t in out["indirect"]}
        for site, t in other["indirect"]:
            targets.setdefault(site, set()).update(t)
        out["indirect"] = [[s, sorted(t)] for s, t in sorted(targets.items())]
    return out


# ---- summary ----------------------------------------------------------------
def losses(health):
    """Trace losses of a session (resyncs of the decoder)."""
    return sum(health.get(k, 0) for k in LOSS_KEYS)


def mismatches(health):
    """Decoded ranges that disagreed with the ELF."""
    return sum(health.get(k, 0) for k in MISMATCH_KEYS)


def is_complete(health):
    """True when no trace was lost and nothing failed a cross-check."""
    return losses(health) == 0 and mismatches(health) == 0


class Counter:
    """covered / total of one metric."""

    __slots__ = ("covered", "total")

    def __init__(self, covered=0, total=0):
        self.covered = covered
        self.total = total

    def add(self, hit, n=1):
        self.total += n
        if hit:
            self.covered += n

    def pct(self):
        return 100.0 * self.covered / self.total if self.total else None

    def pair(self):
        return [self.covered, self.total]


class Line:
    __slots__ = ("number", "insn", "hits", "branches")

    def __init__(self, number):
        self.number = number
        self.insn = Counter()
        self.hits = 0
        self.branches = []  # [taken count, not-taken count, executed] per branch

    def br(self):
        c = Counter()
        for taken, not_taken, _ in self.branches:
            c.add(taken > 0)
            c.add(not_taken > 0)
        return c

    def status(self, complete):
        if self.insn.covered == self.insn.total:
            return "covered"
        if self.insn.covered:
            return "partial"
        return "uncovered" if complete else "not_observed"


class Function:  # pylint: disable=too-few-public-methods
    __slots__ = ("name", "addr", "line", "insn", "branches", "lines", "hits")

    def __init__(self, name, addr, line):
        self.name = name
        self.addr = addr
        self.line = line
        self.insn = Counter()
        self.branches = Counter()
        self.lines = {}  # (file, line) -> covered
        self.hits = 0  # executions of the entry instruction

    def line_counter(self):
        c = Counter()
        for hit in self.lines.values():
            c.add(hit)
        return c


class FileCov:
    __slots__ = ("path", "lines", "functions", "insn", "branches")

    def __init__(self, path):
        self.path = path
        self.lines = {}
        self.functions = []
        self.insn = Counter()
        self.branches = Counter()

    def line_counter(self):
        c = Counter()
        for line in self.lines.values():
            c.add(line.insn.covered > 0)
        return c

    def function_counter(self):
        c = Counter()
        for fn in self.functions:
            c.add(fn.insn.covered > 0)
        return c


class Summary:
    """A coverage file read per file, function and source line."""

    def __init__(self, cov, files, confirm):
        self.cov = cov
        self.files = files  # path -> FileCov, only the selected ones
        self.confirm = confirm
        self.health = cov["health"]
        self.complete = is_complete(self.health)

    @property
    def status(self):
        return "complete" if self.complete else "lossy"

    def sorted_files(self):
        """Files by path, code without source last."""
        return [
            self.files[p]
            for p in sorted(self.files, key=lambda p: (p == UNKNOWN_FILE, p))
        ]

    def totals(self):
        t = {m: Counter() for m in METRICS}
        for fc in self.files.values():
            for key, c in (
                ("lines", fc.line_counter()),
                ("insns", fc.insn),
                ("branches", fc.branches),
                ("functions", fc.function_counter()),
            ):
                t[key].covered += c.covered
                t[key].total += c.total
        return t


UNKNOWN_FILE = "(no source)"


def _selector(include, exclude):
    def selected(path):
        if include and not any(fnmatch.fnmatch(path, g) for g in include):
            return False
        return not any(fnmatch.fnmatch(path, g) for g in exclude)

    return selected


class _Summarizer:
    """Builds a Summary: functions first (they own their instructions), then
    one pass over the instructions."""

    def __init__(self, cov, confirm, include, exclude):
        self.cov = cov
        self.confirm = confirm
        self.selected = _selector(list(include), list(exclude))
        self.addrs = [r[0] for r in cov["insns"]]
        self.branches = {b[0]: b for b in cov["branches"]}
        self.files = {}
        self.owner = [None] * len(self.addrs)  # (FileCov, Function) per insn

    def file_cov(self, fidx):
        path = self.cov["files"][fidx] if fidx != NO_FILE else UNKNOWN_FILE
        if not self.selected(path):
            return None
        if path not in self.files:
            self.files[path] = FileCov(path)
        return self.files[path]

    def covered(self, row):
        return row[3] > 0 and row[4] >= self.confirm

    def outcomes(self, addr, flags):
        """(taken, not taken) counts that pass --confirm, or None when the
        instruction is not a conditional branch."""
        b = self.branches.get(addr)
        if b is None or not (flags & FLAG_WAYPOINT and flags & FLAG_COND):
            return None
        need = self.confirm
        return tuple(
            c if (c and segs >= need) else 0 for c, segs in ((b[1], b[3]), (b[2], b[4]))
        )

    def add_functions(self):
        insns = self.cov["insns"]
        for name, lo, hi, fidx, line in self.cov["functions"]:
            i = bisect.bisect_left(self.addrs, lo)
            j = bisect.bisect_left(self.addrs, hi)
            if i == j:
                continue  # no Thumb code there (data, or not in the code map)
            fc = self.file_cov(fidx)
            fn = None
            if fc is not None:
                fn = Function(name, lo, line)
                fc.functions.append(fn)
                if self.addrs[i] == lo and self.covered(insns[i]):
                    fn.hits = insns[i][3]
            for k in range(i, j):
                self.owner[k] = (fc, fn)  # (None, None): the function is filtered out

    def add_line(self, row, hit, br):
        fidx, line = row[5], row[6]
        lc = self.file_cov(fidx) if fidx != NO_FILE and line else None
        if lc is None:
            return
        ln = lc.lines.get(line)
        if ln is None:
            ln = lc.lines[line] = Line(line)
        ln.insn.add(hit)
        if hit:
            ln.hits = max(ln.hits, row[3])
        if br is not None:
            ln.branches.append((br[0], br[1], hit))

    def add_insns(self):
        for k, row in enumerate(self.cov["insns"]):
            fidx, line = row[5], row[6]
            fc, fn = self.owner[k] or (self.file_cov(fidx), None)
            if fc is None:
                continue
            hit = self.covered(row)
            br = self.outcomes(row[0], row[2])
            fc.insn.add(hit)
            if br is not None:
                fc.branches.add(br[0] > 0)
                fc.branches.add(br[1] > 0)
            if fn is not None:
                fn.insn.add(hit)
                if br is not None:
                    fn.branches.add(br[0] > 0)
                    fn.branches.add(br[1] > 0)
                if fidx != NO_FILE and line:
                    fn.lines[(fidx, line)] = fn.lines.get((fidx, line), False) or hit
            self.add_line(row, hit, br)


def summarize(cov, confirm=1, include=(), exclude=()):
    """Summary of a coverage file. `confirm` is the number of distinct trace
    segments an instruction or branch outcome must be seen in to count."""
    confirm = max(1, int(confirm))
    builder = _Summarizer(cov, confirm, include, exclude)
    builder.add_functions()
    builder.add_insns()
    return Summary(cov, builder.files, confirm)


def parse_thresholds(text):
    """{'lines': 80.0, ...} from 'lines=80,branches=60'."""
    out = {}
    if not text:
        return out
    for part in text.split(","):
        key, sep, value = part.partition("=")
        key = key.strip()
        if not sep or key not in METRICS:
            raise CovError(
                f"bad --fail-under entry {part!r} (want {', '.join(METRICS)}=PERCENT)"
            )
        try:
            out[key] = float(value)
        except ValueError as exc:
            raise CovError(f"bad --fail-under percentage {value!r}") from exc
    return out


def below_thresholds(summary, thresholds):
    """[(metric, percent, wanted)] of the thresholds the summary misses."""
    totals = summary.totals()
    failed = []
    for key, want in thresholds.items():
        pct = totals[key].pct()
        pct = 100.0 if pct is None else pct
        if pct + 1e-9 < want:
            failed.append((key, pct, want))
    return failed
