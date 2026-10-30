"""cortrace profile: profile file, text report, diff and the run pipeline."""

import copy
import json
import os
import sys

import pytest

from cortrace import cli, prof


def hist(count, p50, p99, maximum, *, minimum=0, p90=None, mean=0.0):
    return {
        "count": count,
        "min": minimum,
        "p50": p50,
        "p90": p90 if p90 is not None else p99,
        "p99": p99,
        "max": maximum,
        "mean": mean,
    }


def make_prof():
    return {
        "format": prof.FORMAT,
        "version": prof.VERSION,
        "elf": "/fw/app.elf",
        "clock": {
            "sysclk_hz": 150e6,
            "tsgen_hz": 75e6,
            "cycles": 450_000_000,
            "instructions": 300_000_000,
            "ranges": 80_000_000,
            "cc_packets": 20_000_000,
            "timestamps": 1000,
            "no_instr_cycles": 4500,
            "drift_ppm": 3.2,
        },
        "health": {"nosync": 0, "overflow": 0, "trace_on": 0},
        "functions": [
            {
                "name": "hot",
                "entries": 10,
                "ranges": 20,
                "instructions": 100,
                "cycles": 300.0,
            },
            {
                "name": "cold",
                "entries": 1,
                "ranges": 2,
                "instructions": 50,
                "cycles": 100.0,
            },
            {
                "name": "idle",
                "entries": 0,
                "ranges": 0,
                "instructions": 0,
                "cycles": 0.0,
            },
        ],
        "exceptions": [
            {
                "number": 15,
                "name": "IRQ:SysTick",
                "count": 300,
                "entry": hist(300, 47, 100, 120),
                "duration": hist(300, 289, 3400, 3500),
            }
        ],
        "periodic": {
            "exception": 15,
            "name": "IRQ:SysTick",
            "occurrences": 300,
            "period_cycles": 1_500_000,
            "period_given": False,
            "lateness": hist(300, 18, 3284, 3306, p90=1996),
            "blockers": [
                {
                    "function": "sched",
                    "address": 0x800363A,
                    "count": 70,
                    "min": 50,
                    "mean": 1762.9,
                    "max": 3306,
                }
            ],
        },
        "blocks": [
            {
                "function": "lock",
                "start": 0x8007B7E,
                "end": 0x8007B8A,
                "count": 1000,
                "total": 50000,
                "min": 29,
                "p50": 50,
                "p90": 57,
                "p99": 72,
                "max": 73,
            }
        ],
    }


def with_events(p, sleep_wraps=0):
    """`p` as a trace with DWT event counters on."""
    p["events"] = {
        "packets": 5000,
        "unit": 256,
        "unplaced": 0,
        "sleep_cycles": sleep_wraps * 256,
        "wraps": {"cpi": 100, "exc": 40, "sleep": sleep_wraps, "lsu": 30, "fold": 20},
    }
    shares = {
        "hot": {"cpi": 0, "exc": 30, "sleep": 0, "lsu": 10, "fold": 2},
        "cold": {"cpi": 0, "exc": 10, "sleep": 0, "lsu": 0, "fold": 0},
        "idle": {"cpi": 0, "exc": 0, "sleep": 0, "lsu": 0, "fold": 0},
    }
    for fn in p["functions"]:
        fn["events"] = shares[fn["name"]]
    return p


def write(tmp_path, name, data):
    path = tmp_path / name
    path.write_text(json.dumps(data), encoding="utf-8")
    return str(path)


# ---- the profile file ------------------------------------------------
def test_load_checks_the_file(tmp_path):
    good = write(tmp_path, "p.json", make_prof())
    assert prof.load(good)["elf"] == "/fw/app.elf"
    bad = {
        "missing.json": None,
        "notjson.json": "{",
        "other.json": json.dumps({"format": "cortrace-cov", "version": 1}),
        "list.json": "[]",
        "v9.json": json.dumps({"format": prof.FORMAT, "version": 9}),
    }
    for name, body in bad.items():
        path = tmp_path / name
        if body is not None:
            path.write_text(body, encoding="utf-8")
        with pytest.raises(prof.ProfError):
            prof.load(str(path))


# ---- text report ------------------------------------------------
def test_text_report_sections():
    out = prof.text(make_prof(), top=2)
    assert "Cortrace Cycle Profile" in out
    assert "450,000,000 = 3.0000 s at 150 MHz" in out
    assert "CPI 1.500, IPC 0.667" in out
    assert "+3.2 ppm" in out and "trace health      : clean" in out
    assert "hot" in out and "cold" in out and "idle" not in out  # --top 2
    assert "IRQ:SysTick" in out and "(23.33 us)" in out
    assert "lateness: 300 occurrences, period 1,500,000 cycles (median interval)" in out
    assert "0x0800363a" in out and "sched" in out
    assert "08007b7e-" in out and "the spread is the stall" in out


def test_text_report_without_optional_parts():
    p = make_prof()
    p["clock"].update(sysclk_hz=0.0, drift_ppm=None, cycles=0, instructions=0)
    p["exceptions"] = []
    p["blocks"] = []
    p["periodic"].update(period_cycles=0, blockers=[])
    out = prof.text(p)
    assert "not checked" in out
    assert "us)" not in out and "Exceptions" not in out and "Basic blocks" not in out
    assert "fewer than two occurrences" in out

    q = make_prof()
    q["periodic"].update(period_given=True, blockers=[])
    out = prof.text(q)
    assert "(given)" in out and "was waiting at" not in out


def test_text_report_names_the_losses():
    p = make_prof()
    p["health"] = {"nosync": 2, "overflow": 1, "trace_on": 0}
    out = prof.text(p)
    assert "3 losses (lost sync 2, overflow 1)" in out


# ---- diff ------------------------------------------------
def test_diff_compares_cpi_exceptions_and_lateness():
    a = make_prof()
    b = copy.deepcopy(a)
    b["clock"]["cycles"] = 450_000_000
    b["clock"]["instructions"] = 90_000_000  # slower: 5 cycles per instruction
    b["functions"] = [
        {"name": "hot", "entries": 1, "ranges": 1, "instructions": 50, "cycles": 400.0},
        {"name": "other", "entries": 1, "ranges": 1, "instructions": 0, "cycles": 1.0},
    ]
    b["exceptions"][0]["duration"] = hist(300, 578, 6800, 7000)
    b["exceptions"].append({**a["exceptions"][0], "number": 11, "name": "IRQ:SVCall"})
    a["exceptions"].append({**a["exceptions"][0], "number": 12, "name": "IRQ:gone"})
    out = prof.diff(a, b, top=5)
    assert "CPI          : 1.500 -> 5.000  (3.33x)" in out
    assert "hot" in out and "8.00" in out and "2.67x" in out
    assert "cold" in out
    assert "IRQ:SysTick" in out and "578" in out and "2.00x" in out
    assert "IRQ:gone" not in out and "IRQ:SVCall" not in out
    assert "lateness (cycles): p50 18 -> 18" in out


def test_diff_without_common_data():
    a = make_prof()
    b = make_prof()
    b["exceptions"] = []
    b["functions"] = []
    b["periodic"]["lateness"] = hist(0, 0, 0, 0)
    a["clock"]["instructions"] = 0
    out = prof.diff(a, b)
    assert "Exception run time" not in out and "lateness (cycles)" not in out
    assert "CPI          : 0.000 -> 1.500  (-)" in out
    assert "cold" in out


# ---- report / diff commands ------------------------------------------------
def test_report_and_diff_commands(tmp_path, capsys):
    path = write(tmp_path, "p.json", make_prof())
    assert prof.main(["report", path, "--top", "1"]) == 0
    assert "Cortrace Cycle Profile" in capsys.readouterr().out
    assert prof.main(["report", path, "--format", "json"]) == 0
    assert json.loads(capsys.readouterr().out)["format"] == prof.FORMAT
    out = str(tmp_path / "r.txt")
    assert prof.main(["report", path, "-o", out]) == 0
    assert "Cortrace Cycle Profile" in open(out, encoding="utf-8").read()
    assert "wrote text report" in capsys.readouterr().err

    assert prof.main(["diff", path, path]) == 0
    assert "Cycle Profile Diff" in capsys.readouterr().out
    assert prof.main(["report", str(tmp_path / "none.json")]) == 2
    assert "cannot read" in capsys.readouterr().err


def test_help_and_unknown_command(capsys):
    assert prof.main([]) == 2
    assert prof.main(["--help"]) == 0
    assert "usage: cortrace profile" in capsys.readouterr().out
    assert prof.main(["bogus"]) == 2
    assert "unknown command 'bogus'" in capsys.readouterr().err


def test_cli_routes_profile(monkeypatch):
    seen = {}
    monkeypatch.setattr(prof, "main", lambda args: seen.setdefault("args", args) and 0)
    assert cli.main(["profile", "report", "x.json"]) == 0
    assert seen["args"] == ["report", "x.json"]


# ---- run ------------------------------------------------
FAKE_PROF = """
import json, os, sys
args = sys.argv[1:]
with open(sys.argv[0] + ".args", "w") as f:
    json.dump(args, f)
if "--fail" in open(args[args.index("--raw-in") + 1]).read():
    print("[cortrace-prof] error: no ETM A-sync", file=sys.stderr)
    sys.exit(1)
print("[cortrace-prof] 10 B raw -> 5 B ETM", file=sys.stderr)
with open(args[args.index("--out") + 1], "w") as f:
    f.write(open(os.environ["FAKE_PROF_JSON"]).read())
"""


@pytest.fixture
def fake_tools(tmp_path, monkeypatch):
    """A fake cortrace-prof and a fake nm."""
    script = tmp_path / "fake_prof.py"
    script.write_text(FAKE_PROF)
    wrapper = tmp_path / "cortrace-prof"
    wrapper.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{script}" "$@"\n')
    wrapper.chmod(0o755)
    nm = tmp_path / "fake-nm"
    nm.write_text("#!/bin/sh\necho '08000100 T main'\n")
    nm.chmod(0o755)
    monkeypatch.setenv("CORTRACE_PROF", str(wrapper))
    monkeypatch.setenv("FAKE_PROF_JSON", write(tmp_path, "canned.json", make_prof()))
    monkeypatch.delenv("CORTRACE_OUT_DIR", raising=False)
    monkeypatch.delenv("CORTRACE_IFACE", raising=False)
    return {"nm": str(nm), "args": str(script) + ".args"}


def run_argv(tmp_path, fake_tools, raw, *extra):
    return [
        "run",
        "--elf",
        "fw.elf",
        "--nm",
        fake_tools["nm"],
        "--out-dir",
        str(tmp_path / "out"),
        "--tag",
        "t",
        "--raw-in",
        str(raw),
        *extra,
    ]


def test_run_from_a_raw_file(tmp_path, fake_tools, capsys):
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"\0")
    argv = run_argv(
        tmp_path,
        fake_tools,
        raw,
        "--sysclk-hz",
        "150e6",
        "--tsgen-hz",
        "75e6",
        "--phase",
        "1,0",
    )
    assert prof.main(argv + ["--width", "4", "--period-cycles", "1500000"]) == 0
    with open(fake_tools["args"], encoding="utf-8") as f:
        args = json.load(f)
    assert args[args.index("--raw-in") + 1] == str(raw)
    assert args[args.index("--sysclk-hz") + 1] == "1.5e+08"
    assert args[args.index("--tsgen-hz") + 1] == "7.5e+07"
    assert args[args.index("--phase") + 1] == "1,0"
    assert args[args.index("--period-exception") + 1] == "15"
    assert args[args.index("--period-cycles") + 1] == "1.5e+06"
    assert os.path.isfile(tmp_path / "out" / "prof_t.json")
    cap = capsys.readouterr()
    assert "Cortrace Cycle Profile" in cap.out
    assert "10 B raw -> 5 B ETM" in cap.err and "[cortrace-prof]" not in cap.err

    assert prof.main(argv + ["--format", "json"]) == 0
    assert json.loads(capsys.readouterr().out)["format"] == prof.FORMAT


def test_run_failures(tmp_path, fake_tools, capsys, monkeypatch):
    raw = tmp_path / "raw.bin"
    raw.write_text("--fail")
    assert prof.main(run_argv(tmp_path, fake_tools, raw)) == 2
    err = capsys.readouterr().err
    assert "no ETM A-sync" in err and "cortrace-prof failed" in err
    assert not os.path.exists(tmp_path / "out" / "prof_t.json")

    assert prof.main(run_argv(tmp_path, fake_tools, tmp_path / "none")) == 2
    assert "input not found" in capsys.readouterr().err

    raw.write_text("ok")
    argv = run_argv(tmp_path, fake_tools, raw, "--nm", str(tmp_path / "no-such-nm"))
    assert prof.main(argv) == 2
    assert "cannot get the symbols with" in capsys.readouterr().err

    failing = tmp_path / "failing-nm"
    failing.write_text("#!/bin/sh\nexit 3\n")
    failing.chmod(0o755)
    assert prof.main(run_argv(tmp_path, fake_tools, raw, "--nm", str(failing))) == 2
    assert "cannot get the symbols with" in capsys.readouterr().err

    monkeypatch.setenv("CORTRACE_PROF", str(tmp_path / "no-such-prof"))
    assert prof.main(run_argv(tmp_path, fake_tools, raw)) == 2
    assert "cannot run" in capsys.readouterr().err


def test_run_captures(tmp_path, fake_tools, monkeypatch):
    grabbed = {}

    def fake_grab(a, raw, allow_loss=False):
        assert not allow_loss  # the cycle clock needs every count
        grabbed["iface"] = a.iface
        grabbed["secs"] = a.secs
        with open(raw, "wb") as f:
            f.write(b"\0")

    monkeypatch.setattr(prof.capture, "grab", fake_grab)
    monkeypatch.setattr(prof.runstore, "require_space", lambda *a: None)
    monkeypatch.setenv("CORTRACE_IFACE", "eth9")
    argv = [
        "run",
        "--elf",
        "fw.elf",
        "--nm",
        fake_tools["nm"],
        "--out-dir",
        str(tmp_path / "cap"),
        "--tag",
        "c",
        "--secs",
        "0.5",
    ]
    assert prof.main(argv) == 0
    assert grabbed == {"iface": "eth9", "secs": 0.5}
    assert os.path.isfile(tmp_path / "cap" / "raw_c.bin")
    assert os.path.isfile(tmp_path / "cap" / "prof_c.json")

    with pytest.raises(SystemExit) as e:  # longer than 5 s without --allow-long
        prof.main(argv + ["--secs", "30"])
    assert "--allow-long" in str(e.value)


def test_run_argument_errors(tmp_path, capsys):
    base = ["--elf", "x.elf"]
    for extra, msg in (
        ([], "--out-dir is required"),
        (["--out-dir", str(tmp_path)], "capture NIC not given"),
    ):
        with pytest.raises(SystemExit) as e:
            prof.parse_run_args(base + extra)
        assert e.value.code == 2
        assert msg in capsys.readouterr().err
    a = prof.parse_run_args(base + ["--out-dir", str(tmp_path), "--raw-in", "r"])
    assert a.tag and os.path.isabs(a.elf)


# ---- DWT events ---------------------------------------------------------------------------
def test_text_report_with_event_counters():
    out = prof.text(with_events(make_prof(), sleep_wraps=0))
    assert (
        "DWT events        : 5,000 packets, wraps of 256 (cpi 100, exc 40, sleep 0"
        in out
    )
    assert "CPU load          : never asleep" in out
    assert "LSU%" in out and "MC%" in out and "fold%" in out
    hot = next(line for line in out.splitlines() if line.startswith("  hot "))
    # 10*256 of 300 cycles; 2*256 folded per 100 instructions
    assert hot.split()[-3:] == ["853.3", "0.0", "512.0"]
    assert "Exception overhead (DWT EXCCNT): 10,240 cycles" in out
    assert "75.0%" in out and "25.0%" in out  # hot 30 of 40, cold 10 of 40


def test_text_report_cpu_load_and_the_cycles_asleep():
    p = with_events(make_prof(), sleep_wraps=1_000_000)  # 256 M cycles asleep
    out = prof.text(p)
    assert (
        "CPU load          : 63.7% (busy 450,000,000 cycles, asleep 256,000,000" in out
    )
    assert "(the cycles asleep added)" in out


def test_events_without_exception_overhead_and_old_files():
    p = with_events(make_prof())
    p["events"]["wraps"]["exc"] = 0
    assert "Exception overhead" not in prof.text(p)
    p["events"]["packets"] = 0  # a trace with the counters off
    out = prof.text(p)
    assert "DWT events" not in out and "LSU%" not in out


def test_diff_shows_the_lsu_share_of_both():
    a = with_events(make_prof())
    b = with_events(make_prof())
    b["functions"] = b["functions"][:1]
    b["functions"][0]["events"]["lsu"] = 100
    out = prof.diff(a, b, top=3)
    assert "LSU% A" in out and "LSU% B" in out
    hot = next(line for line in out.splitlines() if line.startswith("  hot "))
    assert hot.split()[-2:] == ["853.3", "8533.3"]
    cold = next(line for line in out.splitlines() if line.startswith("  cold "))
    assert cold.split()[-2:] == ["0.0", "-"]  # not in B
    idle = next(line for line in out.splitlines() if line.startswith("  idle "))
    assert idle.split()[-2:] == ["-", "-"]  # no cycles in A
    plain = prof.diff(make_prof(), with_events(make_prof()))
    assert "LSU%" not in plain
