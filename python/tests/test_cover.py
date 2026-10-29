"""cortrace cover: coverage file, source attribution, reports and the CLI."""

import json
import os
import signal
import sys
import threading
import time
import xml.etree.ElementTree as ET

import elffixture
import pytest

from cortrace import cli, cover, covmodel, covreport

WP_COND = covmodel.FLAG_WAYPOINT | covmodel.FLAG_COND | 1  # conditional direct branch

COMPLETE = {"segments": 10, "committed": 9, "quarantined": 1}
LOSSY = dict(COMPLETE, lost_sync=1)


def make_cov(health=None, sha="a" * 64, tag="t1"):
    """A hand-made coverage file:

    main.c:10   0x100  hit 5 in 3 segments
    main.c:11   0x102  conditional branch, taken 3 times (2 segments), never not taken
    main.c:12   0x104  not hit, 0x106 hit twice in one segment  -> partial line
    util.h:4    0x108  inlined into main, hit once
    util.c:3,4  helper, never hit; 0x112 is a conditional branch
    0x200       a function without source
    main.c:20   0x300  hit, outside every function
    """
    return {
        "format": covmodel.COV_FORMAT,
        "version": covmodel.VERSION,
        "elf": {"path": "/fw/app.elf", "sha256": sha},
        "runs": [{"tag": tag, "captured": "2026-10-09 10:00:00", "secs": 1.0}],
        "health": dict(COMPLETE if health is None else health),
        "files": ["/src/app/main.c", "/src/lib/util.c", "/src/app/util.h"],
        "functions": [
            ["main", 0x100, 0x110, 0, 9],
            ["helper", 0x110, 0x118, 1, 3],
            ["nosrc", 0x200, 0x204, -1, 0],
            ["data_only", 0x400, 0x404, 0, 30],
        ],
        "indirect": [[0x100, [0x110]]],
        "insns": [
            [0x100, 2, 0, 5, 3, 0, 10],
            [0x102, 2, WP_COND, 5, 3, 0, 11],
            [0x104, 2, 0, 0, 0, 0, 12],
            [0x106, 2, 0, 2, 1, 0, 12],
            [0x108, 2, 0, 1, 1, 2, 4],
            [0x110, 2, 0, 0, 0, 1, 3],
            [0x112, 2, WP_COND, 0, 0, 1, 4],
            [0x200, 2, 0, 0, 0, -1, 0],
            [0x300, 2, 0, 1, 1, 0, 20],
        ],
        "branches": [[0x102, 3, 0, 2, 0], [0x112, 0, 0, 0, 0]],
    }


def write(tmp_path, name, cov):
    path = tmp_path / name
    covmodel.save(cov, str(path))
    return str(path)


# ---- summary ----------------------------------------------------------------
def test_summary_counts_lines_insns_branches_functions():
    s = covmodel.summarize(make_cov())
    t = s.totals()
    assert t["insns"].pair() == [5, 9]
    assert t["lines"].pair() == [5, 7]  # main.c 10 11 12 20, util.h 4 / util.c 3 4
    assert t["branches"].pair() == [1, 4]
    assert t["functions"].pair() == [1, 3]  # data_only has no code
    main_c = s.files["/src/app/main.c"]
    assert main_c.lines[12].status(True) == "partial"
    assert main_c.lines[12].hits == 2
    assert main_c.lines[11].br().pair() == [1, 2]
    fn = main_c.functions[0]
    assert (fn.name, fn.hits) == ("main", 5)
    assert fn.line_counter().pair() == [4, 4]  # inlined util.h:4 counts for main
    assert fn.insn.pair() == [4, 5]
    # inlined header code belongs to main.c, its line to util.h
    assert s.files["/src/app/util.h"].insn.total == 0
    assert s.files[covmodel.UNKNOWN_FILE].insn.pair() == [0, 1]
    assert s.sorted_files()[-1].path == covmodel.UNKNOWN_FILE


def test_status_uncovered_vs_not_observed():
    complete = covmodel.summarize(make_cov())
    lossy = covmodel.summarize(make_cov(LOSSY))
    assert complete.status == "complete" and lossy.status == "lossy"
    helper = "/src/lib/util.c"
    assert complete.files[helper].lines[3].status(complete.complete) == "uncovered"
    assert lossy.files[helper].lines[3].status(lossy.complete) == "not_observed"
    assert complete.files["/src/app/main.c"].lines[10].status(True) == "covered"
    mism = covmodel.summarize(make_cov(dict(COMPLETE, flow_mismatch=2)))
    assert not mism.complete


def test_confirm_needs_distinct_segments():
    s = covmodel.summarize(make_cov(), confirm=2)
    t = s.totals()
    # 0x106, 0x108, 0x300 were seen in one segment only
    assert t["insns"].pair() == [2, 9]
    assert s.files["/src/app/main.c"].lines[12].status(True) == "uncovered"
    assert t["branches"].pair() == [1, 4]  # taken seen in 2 segments
    s3 = covmodel.summarize(make_cov(), confirm=3)
    assert s3.totals()["branches"].pair() == [0, 4]
    assert covmodel.summarize(make_cov(), confirm=0).confirm == 1


def test_include_and_exclude_globs():
    s = covmodel.summarize(make_cov(), include=["*/app/*"])
    assert set(s.files) == {"/src/app/main.c", "/src/app/util.h"}
    s = covmodel.summarize(make_cov(), exclude=["*/lib/*", "*/util.h"])
    assert "/src/lib/util.c" not in s.files and "/src/app/util.h" not in s.files
    assert s.totals()["lines"].pair() == [4, 4]
    # excluding a function's file drops its instructions, even inlined ones
    s = covmodel.summarize(make_cov(), exclude=["*/main.c"])
    assert set(s.files) == {"/src/lib/util.c", covmodel.UNKNOWN_FILE}


def test_thresholds():
    assert not covmodel.parse_thresholds(None)
    th = covmodel.parse_thresholds("lines=60, branches=30")
    assert th == {"lines": 60.0, "branches": 30.0}
    s = covmodel.summarize(make_cov())
    failed = covmodel.below_thresholds(s, th)
    assert [f[0] for f in failed] == ["branches"]
    empty = covmodel.summarize(make_cov(), include=["/nothing/*"])
    assert not covmodel.below_thresholds(empty, {"lines": 100})
    for bad in ("lines", "bogus=1", "lines=x"):
        with pytest.raises(covmodel.CovError):
            covmodel.parse_thresholds(bad)


# ---- file I/O and merge ------------------------------------------------------
def test_save_load_roundtrip(tmp_path):
    cov = make_cov()
    path = write(tmp_path, "c.json", cov)
    assert covmodel.load(path) == json.loads(json.dumps(cov))
    with open(path, encoding="utf-8") as f:
        assert len(f.readlines()) > len(cov["insns"])  # one instruction per line


@pytest.mark.parametrize(
    "content,msg",
    [
        ("{", "not JSON"),
        ('{"format": "other", "version": 1}', "not a cortrace-cov file"),
        ('{"format": "cortrace-cov", "version": 99}', "version 99"),
        ("[]", "not a cortrace-cov file"),
    ],
)
def test_load_rejects_bad_files(tmp_path, content, msg):
    path = tmp_path / "bad.json"
    path.write_text(content)
    with pytest.raises(covmodel.CovError, match=msg):
        covmodel.load(str(path))


def test_load_missing_file(tmp_path):
    with pytest.raises(covmodel.CovError, match="No such file"):
        covmodel.load(str(tmp_path / "none.json"))


def test_merge_adds_counts_health_runs_and_targets():
    a = make_cov(LOSSY)
    b = make_cov(tag="t2")
    b["indirect"] = [[0x100, [0x120]], [0x130, [0x140]]]
    m = covmodel.merge([a, b])
    assert m["insns"][0][3:5] == [10, 6]
    assert m["branches"][0] == [0x102, 6, 0, 4, 0]
    assert m["health"]["segments"] == 20 and m["health"]["lost_sync"] == 1
    assert [r["tag"] for r in m["runs"]] == ["t1", "t2"]
    assert m["indirect"] == [[0x100, [0x110, 0x120]], [0x130, [0x140]]]
    assert a["insns"][0][3] == 5  # inputs untouched
    # merged runs reach --confirm 2 where one run alone does not
    assert covmodel.summarize(m, confirm=2).totals()["insns"].covered == 5


def test_merge_refuses_mixed_inputs():
    with pytest.raises(covmodel.CovError, match="nothing"):
        covmodel.merge([])
    with pytest.raises(covmodel.CovError, match="different ELFs"):
        covmodel.merge([make_cov(), make_cov(sha="b" * 64, tag="t2")])
    with pytest.raises(covmodel.CovError, match="same run"):
        covmodel.merge([make_cov(), make_cov()])
    short = make_cov(tag="t2")
    short["insns"].pop()
    with pytest.raises(covmodel.CovError, match="disagree"):
        covmodel.merge([make_cov(), short])


# ---- source attribution from a real ELF --------------------------------------
SOURCE = """int g;
int leaf(int x)
{
  if (x > 3)
    return x * 2;
  return x + 1;
}
void main(void)
{
  g = leaf(g);
}
"""


def row_starts(elf):
    """Start address of every line-table row of `elf`."""
    from elftools.elf.elffile import ELFFile

    with open(elf, "rb") as f:
        rows = covmodel.line_rows(ELFFile(f).get_dwarf_info(), covmodel._FileTable())
    return sorted({r[0] for r in rows})


@pytest.mark.parametrize("dwarf", ["-gdwarf-4", "-gdwarf-5"])
def test_attach_source_from_dwarf(tmp_path, dwarf):
    elf = elffixture.build(tmp_path, "fw", SOURCE, "-O0", dwarf)
    obj = {
        "format": covmodel.OBJ_FORMAT,
        "version": 1,
        "commit_lag": 8,
        "health": dict(COMPLETE),
        "insns": [[0x10, 2, 0, 0, 0]] + [[a, 2, 0, 1, 1] for a in row_starts(elf)],
        "branches": [],
        "indirect": [],
    }
    cov = covmodel.attach_source(obj, elf, {"tag": "x"})
    src = os.path.normpath(str(tmp_path / "fw.c"))
    assert src in cov["files"]
    fidx = cov["files"].index(src)
    lines = {r[6] for r in cov["insns"] if r[5] == fidx}
    assert {3, 4, 5, 6, 9, 10} <= lines <= set(range(1, 12))
    assert cov["insns"][0][5:] == [covmodel.NO_FILE, 0]
    funcs = {f[0]: f for f in cov["functions"]}
    # the entry row of a function is its opening line
    assert funcs["leaf"][3] == fidx and funcs["leaf"][4] in (2, 3)
    assert funcs["main"][4] in (8, 9)
    assert cov["elf"]["sha256"] == covmodel.sha256_file(elf)
    assert cov["runs"][0]["commit_lag"] == 8
    s = covmodel.summarize(cov)
    names = sorted(fn.name for fn in s.files[src].functions)
    assert names == ["leaf", "main"]
    assert s.files[src].function_counter().pair() == [2, 2]


def test_function_symbols_prefer_global_and_fill_sizes():
    class Sym:
        def __init__(self, name, value, size, *, bind="STB_GLOBAL", kind="STT_FUNC"):
            self.name = name
            self._d = {
                "st_value": value,
                "st_size": size,
                "st_shndx": 1,
                "st_info": {"type": kind, "bind": bind},
            }

        def __getitem__(self, key):
            return self._d[key]

    class Elf:
        def __init__(self, syms):
            self.syms = syms

        def get_section_by_name(self, _name):
            return self if self.syms is not None else None

        def iter_symbols(self):
            return iter(self.syms)

    syms = [
        Sym("WWDG_IRQHandler", 0x201, 0, bind="STB_WEAK"),
        Sym("Default_Handler", 0x201, 4),
        Sym("lonely", 0x301, 0, bind="STB_LOCAL"),
        Sym("", 0x401, 2),
        Sym("var", 0x500, 4, kind="STT_OBJECT"),
    ]
    got = covmodel.function_symbols(Elf(syms))
    assert got == [(0x200, 0x204, "Default_Handler"), (0x300, 0x302, "lonely")]
    syms.append(Sym("first", 0x101, 0))
    assert covmodel.function_symbols(Elf(syms))[0] == (0x100, 0x200, "first")
    assert not covmodel.function_symbols(Elf(None))


def test_row_index_backtracks_over_overlaps():
    rows = [(0, 100, 0, 1), (0, 4, 1, 2), (4, 8, 1, 3)]
    idx = covmodel._RowIndex(sorted(rows))
    assert idx.at(6) == (1, 3)
    assert idx.at(50) == (0, 1)  # found behind the later-starting (4, 8) row
    assert idx.at(100) == (covmodel.NO_FILE, 0)


# ---- reports -----------------------------------------------------------------
def test_text_report():
    s = covmodel.summarize(make_cov(LOSSY))
    out = covreport.text(s)
    assert "Sources: /src" in out
    assert "app/main.c" in out and "  main" in out
    assert "not observed" in out and "-> lossy" in out
    assert "1 trace losses" in out
    assert "Functions: 1 / 3" in out
    assert "duration: 1 s" in out
    short = covreport.text(covmodel.summarize(make_cov()), functions=False)
    assert "  main " not in short and "not observed" not in short


def test_text_report_shortens_long_paths():
    cov = make_cov()
    cov["files"][0] = "/x/" + "d" * 80 + "/main.c"
    out = covreport.text(covmodel.summarize(cov), src_root="/elsewhere")
    assert "...ddd" in out and "Sources: /elsewhere" in out


def test_json_report():
    rep = covreport.to_json(covmodel.summarize(make_cov()), src_root="/src")
    assert rep["status"] == "complete" and rep["totals"]["insns"] == [5, 9]
    main_c = next(f for f in rep["files"] if f["path"] == "app/main.c")
    by_line = {ln["line"]: ln for ln in main_c["line_detail"]}
    assert by_line[12]["status"] == "partial" and by_line[12]["insn"] == [1, 2]
    assert by_line[11]["br"] == [1, 2]
    assert main_c["function_detail"][0]["name"] == "main"


def test_lcov_report():
    out = covreport.lcov(covmodel.summarize(make_cov()), test_name="t1")
    records = out.split("end_of_record\n")
    main_c = next(r for r in records if "SF:/src/app/main.c" in r)
    assert "FN:9,main" in main_c and "FNDA:5,main" in main_c
    assert "DA:12,2" in main_c and "DA:10,5" in main_c
    assert "BRDA:11,0,0,3" in main_c and "BRDA:11,0,1,0" in main_c
    assert "LF:4" in main_c and "LH:4" in main_c
    util_c = next(r for r in records if "SF:/src/lib/util.c" in r)
    assert "BRDA:4,0,0,-" in util_c and "FNDA:0,helper" in util_c
    assert covmodel.UNKNOWN_FILE not in out
    assert covreport.lcov(covmodel.summarize(make_cov(), include=["/no/*"])) == ""


def test_cobertura_report():
    xml = covreport.cobertura(covmodel.summarize(make_cov()), src_root="/src")
    root = ET.fromstring(xml)
    assert root.get("lines-valid") == "7" and root.get("lines-covered") == "5"
    classes = {c.get("filename"): c for c in root.iter("class")}
    assert set(classes) == {"app/main.c", "app/util.h", "lib/util.c"}
    line11 = classes["app/main.c"].find("lines/line[@number='11']")
    assert line11.get("condition-coverage") == "50% (1/2)"
    method = classes["app/main.c"].find("methods/method")
    assert method.get("name") == "main"
    assert [ln.get("number") for ln in method.iter("line")] == ["10", "11", "12"]
    absolute = ET.fromstring(covreport.cobertura(covmodel.summarize(make_cov())))
    assert any(c.get("filename") == "src/app/main.c" for c in absolute.iter("class"))


def test_display_and_common_root():
    assert covreport.display_path("/a/b/c.c", "/a") == "b/c.c"
    assert covreport.display_path("/x/c.c", "/a") == "/x/c.c"
    assert covreport.display_path("rel.c", "/a") == "rel.c"
    assert covreport.common_root(["/a/b/c.c", "/a/d.c"]) == "/a"
    assert covreport.common_root(["/a.c", "/b.c"]) == ""
    assert covreport.common_root(["(no source)"]) == ""


# ---- CLI ---------------------------------------------------------------------
def test_report_command_formats_and_gates(tmp_path, capsys):
    path = write(tmp_path, "c.json", make_cov(LOSSY))
    assert cover.main(["report", path]) == 0
    assert "Cortrace Code Coverage Report" in capsys.readouterr().out
    for fmt in ("json", "lcov", "cobertura"):
        out = str(tmp_path / f"r.{fmt}")
        assert cover.main(["report", path, "--format", fmt, "-o", out]) == 0
        assert os.path.getsize(out) > 0
    capsys.readouterr()
    assert cover.main(["report", path, "--require-lossless"]) == 1
    assert "FAIL --require-lossless" in capsys.readouterr().err
    assert cover.main(["report", path, "--fail-under", "branches=50"]) == 1
    assert "branches 25.0% < 50%" in capsys.readouterr().err
    assert cover.main(["report", path, "--fail-under", "lines=50"]) == 0
    assert cover.main(["report", path, "--fail-under", "nope=1"]) == 2
    assert cover.main(["report", str(tmp_path / "missing.json")]) == 2


def test_merge_and_diff_commands(tmp_path, capsys):
    a = write(tmp_path, "a.json", make_cov(tag="a"))
    b_cov = make_cov(tag="b")
    b_cov["insns"][0][3:5] = [0, 0]  # main.c:10 no longer covered
    b_cov["insns"][5][3:5] = [1, 1]  # util.c:3 newly covered
    b = write(tmp_path, "b.json", b_cov)
    out = str(tmp_path / "m.json")
    assert cover.main(["merge", a, b, "-o", out]) == 0
    assert len(covmodel.load(out)["runs"]) == 2
    assert "merged 2 files" in capsys.readouterr().err
    assert cover.main(["merge", a, a, "-o", out]) == 2

    assert cover.main(["diff", a, b, "--src-root", "/src", "--limit", "0"]) == 0
    text = capsys.readouterr().out
    assert "Lines no longer covered: 1" in text and "... 1 more" in text
    assert "Lines newly covered: 1" in text
    assert cover.main(["diff", a, b, "--fail-on-regression"]) == 1
    assert (
        "  /src/app/main.c:10" in capsys.readouterr().out
    )  # absolute without --src-root
    assert cover.main(["diff", a, a, "--fail-on-regression"]) == 0
    lossy = write(tmp_path, "l.json", make_cov(LOSSY, tag="l"))
    cover.main(["diff", a, lossy])
    assert "no longer observed" in capsys.readouterr().out


def test_diff_text_without_totals():
    empty = covmodel.summarize(make_cov(), include=["/none/*"])
    text, regressions = cover.diff_text(empty, empty)
    assert regressions == 0 and "Lines" in text


def test_help_and_unknown_command(capsys):
    assert cover.main([]) == 2
    assert cover.main(["--help"]) == 0
    assert "usage: cortrace cover" in capsys.readouterr().out
    assert cover.main(["bogus"]) == 2
    assert "unknown command 'bogus'" in capsys.readouterr().err


def test_cli_routes_cover(monkeypatch):
    seen = {}
    monkeypatch.setattr(cover, "main", lambda args: seen.setdefault("args", args) and 0)
    assert cli.main(["cover", "report", "x.json"]) == 0
    assert seen["args"] == ["report", "x.json"]
    assert "cover" in cli.SUMMARIES


# ---- cover run ---------------------------------------------------------------
FAKE_DECODE = """
import json, sys
args = sys.argv[1:]
out = args[args.index("--cov-out") + 1]
with open(sys.argv[0] + ".args", "w") as f:
    json.dump(args, f)
if "--fail" in open(args[0]).read():
    sys.exit(3)
print("=== code coverage (clean segments only) ===", file=sys.stderr)
print("  instructions hit    : 1 / 2", file=sys.stderr)
print("  noise", file=sys.stderr)
json.dump({"format": "cortrace-objcov", "version": 1, "commit_lag": 4,
           "health": {"segments": 3}, "insns": [[16, 2, 0, 1, 1], [18, 2, 0, 0, 0]],
           "branches": [], "indirect": []}, open(out, "w"))
"""


@pytest.fixture
def fake_tools(tmp_path, monkeypatch):
    """A fake cortrace-decode and an ELF without debug info."""
    decoder = tmp_path / "fake_decode.py"
    decoder.write_text(FAKE_DECODE)
    wrapper = tmp_path / "cortrace-decode"
    wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{decoder}" "$@"\n')
    wrapper.chmod(0o755)
    monkeypatch.setenv("CORTRACE_DECODE", str(wrapper))
    monkeypatch.delenv("CORTRACE_OUT_DIR", raising=False)
    monkeypatch.delenv("CORTRACE_IFACE", raising=False)
    elf = elffixture.build(tmp_path, "nodebug", SOURCE, "-g0")
    nm = elffixture.shutil.which("nm")
    if nm is None:
        pytest.skip("no nm")
    return {"elf": elf, "nm": nm, "args": str(decoder) + ".args"}


def test_run_from_etm_file(tmp_path, fake_tools, capsys):
    etm = tmp_path / "etm.bin"
    etm.write_bytes(b"\0" * 16)
    out = tmp_path / "out"
    argv = ["run", "--elf", fake_tools["elf"], "--nm", fake_tools["nm"]]
    argv += [
        "--etm-in",
        str(etm),
        "--out-dir",
        str(out),
        "--tag",
        "e",
        "--cov-lag",
        "4",
    ]
    assert cover.main(argv + ["--format", "lcov"]) == 0
    with open(fake_tools["args"], encoding="utf-8") as f:
        args = json.load(f)
    assert "--raw" not in args and args[args.index("--cov-lag") + 1] == "4"
    cov = covmodel.load(str(out / "cov_e.json"))
    assert cov["runs"][0]["tag"] == "e" and cov["runs"][0]["secs"] is None
    assert cov["runs"][0]["trace"] == "etm.bin" and cov["files"] == []
    assert not os.path.exists(out / "objcov_e.json")
    err = capsys.readouterr().err
    assert "instructions hit" in err and "noise" not in err


def test_run_decode_failure(tmp_path, fake_tools, capsys):
    raw = tmp_path / "raw.bin"
    raw.write_text("--fail")
    argv = ["run", "--elf", fake_tools["elf"], "--nm", fake_tools["nm"], "--tag", "f"]
    argv += ["--raw-in", str(raw), "--out-dir", str(tmp_path / "o"), "--phase", "1,0"]
    assert cover.main(argv) == 2
    assert "decode failed" in capsys.readouterr().err
    with open(fake_tools["args"], encoding="utf-8") as f:
        args = json.load(f)
    assert args[args.index("--phase") + 1] == "1,0" and "--raw" in args
    argv[argv.index("--raw-in") + 1] = str(tmp_path / "none")
    assert cover.main(argv) == 2
    assert "input not found" in capsys.readouterr().err


def test_run_captures(tmp_path, fake_tools, monkeypatch):
    grabbed = {}

    def fake_grab(a, raw, allow_loss=False):
        assert allow_loss  # a lossy capture still gives clean segments
        grabbed["iface"] = a.iface
        with open(raw, "wb") as f:
            f.write(b"\0")

    monkeypatch.setattr(cover.capture, "grab", fake_grab)
    monkeypatch.setattr(cover.runstore, "require_space", lambda *a: None)
    monkeypatch.setenv("CORTRACE_IFACE", "eth9")
    out = tmp_path / "cap"
    argv = [
        "run",
        "--elf",
        fake_tools["elf"],
        "--nm",
        fake_tools["nm"],
        "--secs",
        "0.5",
    ]
    assert cover.main(argv + ["--out-dir", str(out), "--tag", "c"]) == 0
    assert grabbed["iface"] == "eth9"
    assert covmodel.load(str(out / "cov_c.json"))["runs"][0]["secs"] == 0.5
    assert (out / "raw_c.bin").exists()


def test_run_argument_errors(tmp_path, capsys):
    base = ["--elf", "x.elf"]
    for extra, msg in (
        ([], "--out-dir is required"),
        (["--out-dir", str(tmp_path)], "capture NIC not given"),
        (["--out-dir", str(tmp_path), "--raw-in", "r", "--cov-lag", "-1"], "--cov-lag"),
    ):
        with pytest.raises(SystemExit) as e:
            cover.parse_run_args(base + extra)
        assert e.value.code == 2
        assert msg in capsys.readouterr().err
    a = cover.parse_run_args(base + ["--out-dir", str(tmp_path), "--etm-in", "e"])
    assert a.tag and os.path.isabs(a.elf)


# ---- cover stream ------------------------------------------------------------
FAKE_COV = """
import json, os, signal, sys, time
args = sys.argv[1:]
out = args[args.index("--out") + 1]
with open(sys.argv[0] + ".args", "w") as f:
    json.dump(args, f)
mode = os.environ.get("FAKE_COV_MODE", "ok")
if mode == "fail":
    print("cortrace-cov: no ETM A-sync", file=sys.stderr)
    sys.exit(3)
if mode == "nofile":
    sys.exit(0)
stop = []
signal.signal(signal.SIGINT, lambda *_: stop.append(1))
signal.signal(signal.SIGTERM, lambda *_: stop.append(1))
if mode == "wait":  # a live capture: runs until it is told to stop
    open(sys.argv[0] + ".ready", "w").close()
    while not stop:
        time.sleep(0.02)
json.dump({"format": "cortrace-objcov", "version": 1, "commit_lag": 8,
           "health": {"segments": 5, "committed": 4, "quarantined": 1, "udp_gaps": 1},
           "insns": [[16, 2, 0, 3, 2], [18, 2, 0, 0, 0]],
           "branches": [], "indirect": []}, open(out, "w"))
"""


@pytest.fixture
def fake_cov(tmp_path, monkeypatch):
    """A fake cortrace-cov and an ELF without debug info."""
    script = tmp_path / "fake_cov.py"
    script.write_text(FAKE_COV)
    wrapper = tmp_path / "cortrace-cov"
    wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n')
    wrapper.chmod(0o755)
    monkeypatch.setenv("CORTRACE_COV", str(wrapper))
    monkeypatch.delenv("CORTRACE_OUT_DIR", raising=False)
    monkeypatch.delenv("CORTRACE_IFACE", raising=False)
    monkeypatch.delenv("FAKE_COV_MODE", raising=False)
    elf = elffixture.build(tmp_path, "nodebug", SOURCE, "-g0")
    return {"elf": elf, "args": str(script) + ".args", "ready": str(script) + ".ready"}


def test_stream_runs_cortrace_cov_and_reports(tmp_path, fake_cov, capsys):
    out = tmp_path / "out"
    argv = ["stream", "--elf", fake_cov["elf"], "--out-dir", str(out), "--tag", "s"]
    argv += ["--iface", "eth7", "--secs", "90", "--workers", "6", "--cov-lag", "5"]
    argv += ["--phase", "1,0", "--format", "json"]
    assert cover.main(argv) == 0
    with open(fake_cov["args"], encoding="utf-8") as f:
        args = json.load(f)
    assert args[args.index("--iface") + 1] == "eth7"
    assert args[args.index("--secs") + 1] == "90"
    assert args[args.index("--workers") + 1] == "6"
    assert args[args.index("--cov-lag") + 1] == "5"
    assert args[args.index("--phase") + 1] == "1,0"
    assert "--raw-in" not in args
    cov = covmodel.load(str(out / "cov_s.json"))
    assert cov["runs"][0]["tag"] == "s" and cov["runs"][0]["trace"] == "stream"
    assert cov["health"]["udp_gaps"] == 1
    assert not os.path.exists(out / "objcov_s.json")  # only the coverage file is kept
    assert sorted(os.listdir(out)) == ["cov_s.json"]  # and the trace was never stored
    report = json.loads(capsys.readouterr().out)
    assert report["status"] == "lossy"  # a UDP gap is a loss


def test_stream_replays_a_file_and_stops_on_a_signal(tmp_path, fake_cov, monkeypatch):
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"\0")
    out = tmp_path / "o"
    argv = ["stream", "--elf", fake_cov["elf"], "--out-dir", str(out), "--tag", "r"]
    assert cover.main(argv + ["--raw-in", str(raw), "--require-lossless"]) == 1
    with open(fake_cov["args"], encoding="utf-8") as f:
        args = json.load(f)
    assert args[args.index("--raw-in") + 1] == str(raw)
    assert "--iface" not in args and "--secs" not in args

    # a capture with no end: Ctrl-C (here SIGTERM to us) stops it and the
    # report is still made from what it saw
    monkeypatch.setenv("FAKE_COV_MODE", "wait")
    done = {}

    def stopper():
        for _ in range(500):
            if os.path.exists(fake_cov["ready"]):
                break
            time.sleep(0.02)
        os.kill(os.getpid(), signal.SIGTERM)
        done["sent"] = True

    th = threading.Thread(target=stopper)
    th.start()
    code = cover.main(argv + ["--raw-in", str(raw), "--tag", "w", "--no-functions"])
    th.join()
    assert done.get("sent") and code == 0
    assert os.path.isfile(out / "cov_w.json")


def test_stream_failures(tmp_path, fake_cov, monkeypatch, capsys):
    base = ["stream", "--elf", fake_cov["elf"], "--out-dir", str(tmp_path / "o")]
    base += ["--raw-in", str(tmp_path / "r"), "--tag", "f"]
    monkeypatch.setenv("FAKE_COV_MODE", "fail")
    assert cover.main(base) == 2
    assert "exit code 3" in capsys.readouterr().err
    monkeypatch.setenv("FAKE_COV_MODE", "nofile")
    assert cover.main(base) == 2  # it said it was fine but wrote nothing
    assert not os.path.exists(tmp_path / "o" / "cov_f.json")
    monkeypatch.setenv("FAKE_COV_MODE", "ok")
    assert cover.main(base[:2] + ["/missing.elf"] + base[3:]) == 2
    assert "ELF not found" in capsys.readouterr().err
    monkeypatch.setenv("CORTRACE_COV", str(tmp_path / "no-such-binary"))
    assert cover.main(base) == 2
    assert "cannot run" in capsys.readouterr().err


def test_stream_argument_errors(tmp_path, capsys):
    base = ["--elf", "x.elf"]
    for extra, msg in (
        ([], "--out-dir is required"),
        (["--out-dir", str(tmp_path)], "capture NIC not given"),
        (["--out-dir", str(tmp_path), "--raw-in", "r", "--secs", "-1"], "--secs"),
        (["--out-dir", str(tmp_path), "--raw-in", "r", "--chunk-kb", "1"], "chunk-kb"),
    ):
        with pytest.raises(SystemExit) as e:
            cover.parse_stream_args(base + extra)
        assert e.value.code == 2
        assert msg in capsys.readouterr().err
    with pytest.raises(SystemExit):  # one source only
        cover.parse_stream_args(
            base + ["--out-dir", str(tmp_path), "--iface", "e", "--raw-in", "r"]
        )
    a = cover.parse_stream_args(base + ["--out-dir", str(tmp_path), "--iface", "e"])
    assert a.tag and a.secs == 0 and os.path.isabs(a.elf)
    cmd = cover.stream_command(a, "o.json")
    assert "--secs" not in cmd and "--rcvbuf-mb" in cmd


def test_losses_include_the_streaming_counters():
    for key in ("udp_gaps", "dropped_chunks", "decode_fatal"):
        assert covmodel.losses({key: 2}) == 2
    assert not covmodel.is_complete({"dropped_chunks": 1})
    assert covmodel.is_complete({"udp_lost_frames": 40, "dropped_bytes": 9})
