import json

import pytest

from cortrace import cli, stats


def fn(name, calls=1, incl=100, self_=100, **kw):
    f = {
        "name": name,
        "calls": calls,
        "crossed": 0,
        "truncated": 0,
        "incl": incl,
        "self": self_,
        "min": 1,
        "p50": 10,
        "p90": 20,
        "p99": 30,
        "max": 40,
    }
    f.update(kw)
    return f


def make_stats(**kw):
    s = {
        "format": stats.FORMAT,
        "version": stats.VERSION,
        "elf": "fw.elf",
        "unit": "cycles",
        "sysclk_hz": 150e6,
        "health": {
            "credited": 1000,
            "dropped": 0,
            "losses": 0,
            "crossed_frames": 0,
            "truncated_frames": 2,
        },
        "functions": [
            fn("main", 1, 1000, 100),
            fn("work", 10, 700, 500),
            fn("leaf", 100, 200, 200, crossed=3, truncated=1),
        ],
        "edges": [
            {"caller": "<root>", "callee": "main", "calls": 1, "incl": 1000},
            {"caller": "main", "callee": "work", "calls": 10, "incl": 700},
            {"caller": "work", "callee": "leaf", "calls": 100, "incl": 200},
        ],
        "threads": [
            {
                "id": 1,
                "name": "worker",
                "run": 900,
                "isr": 100,
                "isr_entries": 4,
                "intervals": 5,
                "longest": 300,
                "lossy_intervals": 1,
            }
        ],
    }
    s.update(kw)
    return s


def write(tmp_path, name, data):
    p = tmp_path / name
    p.write_text(json.dumps(data), encoding="utf-8")
    return str(p)


def test_load_checks_the_file(tmp_path):
    assert stats.load(write(tmp_path, "s.json", make_stats()))["elf"] == "fw.elf"
    with pytest.raises(stats.StatsError, match="cannot read"):
        stats.load(str(tmp_path / "missing.json"))
    with pytest.raises(stats.StatsError, match="not a cortrace-callstats"):
        stats.load(write(tmp_path, "p.json", {"format": "cortrace-prof"}))
    with pytest.raises(stats.StatsError, match="version 9"):
        stats.load(write(tmp_path, "v.json", make_stats(version=9)))


def test_text_report_sections():
    out = stats.text(make_stats())
    assert "observed   : 1,000 cycles = 0.01 ms at 150 MHz" in out
    assert "lost trace : none" in out
    assert "open at end: 2 calls" in out
    assert "worker" in out and "isr entries" in out
    lines = [ln for ln in out.splitlines() if ln.startswith("  work ")]
    assert lines and "500" in lines[0] and " 50.0%" in lines[0]
    assert "call edges" not in out  # only on request


def test_functions_sort_by_self_incl_or_calls():
    def first(sort):
        out = stats.text(make_stats(), top=1, sort=sort)
        table = out.split("functions by")[1].splitlines()
        return table[2].split()[0]  # [0] is the title, [1] the column heads

    assert first("self") == "work"
    assert first("incl") == "main"
    assert first("calls") == "leaf"


def test_top_limits_the_functions():
    out = stats.text(make_stats(), top=2)
    assert "top 2 of 3" in out
    assert "main" not in out.split("functions by")[1]  # the smallest self time


def test_lost_trace_is_reported():
    s = make_stats()
    s["health"].update(dropped=3000, losses=7, crossed_frames=5)
    out = stats.text(s)
    assert "7 events" in out and "3,000 cycles dropped" in out
    assert "75.0% of the trace" in out
    assert "5 calls spanned it" in out


def test_edges_on_request():
    out = stats.text(make_stats(), edges=2)
    assert "call edges by inclusive time (top 2)" in out
    assert "<root>" in out and "main" in out


def test_microseconds_need_the_cpu_clock():
    out = stats.text(make_stats(), unit="us")
    assert "self (us)" in out and "3.33" in out  # 500 cycles at 150 MHz
    with pytest.raises(stats.StatsError, match="--sysclk-hz"):
        stats.text(make_stats(sysclk_hz=None), unit="us")


def test_function_view_lists_callers_and_callees():
    out = stats.text(make_stats(), func="work")
    assert any(ln.startswith("work: 10 calls") for ln in out.splitlines())
    assert "called from:" in out and "calls:" in out
    callers = out.split("called from:")[1].split("calls:")[0]
    assert "main" in callers and "100.0%" in callers
    callees = out.split("\ncalls:")[1]
    assert "leaf" in callees and " 28.6%" in callees  # 200 of 700


def test_function_view_notes_what_was_not_counted_and_finds_substrings():
    out = stats.text(make_stats(), func="lea")
    assert "3 calls spanned lost trace, 1 were still open" in out
    with pytest.raises(stats.StatsError, match="no function named"):
        stats.text(make_stats(), func="nope")
    with pytest.raises(stats.StatsError, match="matches 2 functions"):
        stats.text(make_stats(), func="a")  # main, leaf


def test_report_command(tmp_path, capsys):
    path = write(tmp_path, "s.json", make_stats())
    assert stats.main(["report", path, "--top", "5", "--edges", "3"]) == 0
    assert "functions by self" in capsys.readouterr().out
    out = tmp_path / "r.json"
    assert stats.main(["report", path, "--format", "json", "-o", str(out)]) == 0
    assert json.loads(out.read_text())["format"] == stats.FORMAT
    assert stats.main(["report", path, "--func", "work"]) == 0
    assert "called from:" in capsys.readouterr().out


def test_report_errors_are_exit_code_2(tmp_path, capsys):
    assert stats.main(["report", str(tmp_path / "nope.json")]) == 2
    assert "cortrace stats: cannot read" in capsys.readouterr().err
    path = write(tmp_path, "s.json", make_stats(sysclk_hz=None))
    assert stats.main(["report", path, "--unit", "us"]) == 2


FOLDED = "main;a;b 30\nmain;a 20\nmain;c 50\n"


def test_folded_stacks_become_a_tree():
    root = stats.parse_folded(FOLDED.splitlines(True))
    assert root.total == 100
    main = root.children["main"]
    assert main.total == 100 and main.self_ == 0
    assert main.children["a"].total == 50 and main.children["a"].self_ == 20
    assert main.children["a"].children["b"].self_ == 30
    assert stats.depth(root) == 4


def test_folded_names_may_contain_spaces_but_the_count_must_be_last():
    root = stats.parse_folded(["my thread (pid 3);f 7\n"])
    assert root.children["my thread (pid 3)"].children["f"].self_ == 7
    for bad in ("main;a\n", "main;a x\n", " 5\n"):
        with pytest.raises(stats.StatsError, match="folded line 1"):
            stats.parse_folded([bad])


def test_flame_svg_has_a_box_and_a_tooltip_per_frame():
    svg = stats.flame_svg(stats.parse_folded(FOLDED.splitlines(True)), title="t")
    assert svg.startswith("<svg") and svg.rstrip().endswith("</svg>")
    for name in ("main", "a", "b", "c"):
        assert f"<title>{name}\n" in svg
    assert "50.00%" in svg  # c
    assert "self 30 cycles" in svg


def test_flame_svg_widths_follow_the_cycles():
    svg = stats.flame_svg(
        stats.parse_folded(FOLDED.splitlines(True)), width=1016, min_pct=0
    )
    import re

    widths = {
        m.group(1): float(m.group(2))
        for m in re.finditer(r"<title>(\w+)\n.*?width=\"([\d.]+)\"", svg, re.S)
    }
    assert widths["main"] == pytest.approx(1000)
    assert widths["c"] == pytest.approx(500)
    assert widths["b"] == pytest.approx(300)


def test_flame_svg_leaves_out_tiny_frames_and_escapes_names():
    folded = ["main;<tiny>&x 1\n", "main;big 100000\n"]
    svg = stats.flame_svg(stats.parse_folded(folded), min_pct=1)
    assert "big" in svg and "tiny" not in svg
    svg = stats.flame_svg(stats.parse_folded(folded), min_pct=0)
    assert "&lt;tiny&gt;&amp;x" in svg and "<tiny>" not in svg


def test_flame_svg_tooltips_show_microseconds_with_a_clock():
    svg = stats.flame_svg(stats.parse_folded(["main 150\n"]), hz=150e6)
    assert "1.00 us" in svg


def test_flame_needs_some_cycles():
    with pytest.raises(stats.StatsError, match="no cycles"):
        stats.flame_svg(stats.parse_folded(["main 0\n"]))


def test_flame_command(tmp_path, capsys):
    folded = tmp_path / "f.txt"
    folded.write_text(FOLDED, encoding="utf-8")
    out = tmp_path / "flame.svg"
    assert stats.main(["flame", str(folded), "-o", str(out), "--title", "run 1"]) == 0
    assert "run 1" in out.read_text()
    assert stats.main(["flame", str(tmp_path / "none.txt"), "-o", str(out)]) == 2
    assert "cannot read" in capsys.readouterr().err


def test_usage(capsys):
    assert stats.main([]) == 2
    assert "usage: cortrace stats" in capsys.readouterr().out
    assert stats.main(["--help"]) == 0
    assert stats.main(["frob"]) == 2
    assert "unknown command" in capsys.readouterr().err


def test_cli_routes_stats(monkeypatch):
    seen = {}
    monkeypatch.setattr(stats, "main", lambda a: seen.update(args=a) or 0)
    assert cli.main(["stats", "report", "x.json"]) == 0
    assert seen["args"] == ["report", "x.json"]


def test_very_deep_stacks_do_not_hit_the_recursion_limit():
    # recursive code (a tree walk) makes folded stacks hundreds of frames deep
    deep = ";".join(f"f{i % 2}" for i in range(3000)) + " 10\n"
    root = stats.parse_folded([deep])
    assert stats.depth(root) == 3001
    svg = stats.flame_svg(root, min_pct=0)
    assert svg.count("<rect") == 3002  # background + 3001 frames
