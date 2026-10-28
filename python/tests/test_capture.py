import types

import pytest

from cortrace import capture


def parse(*args):
    return capture.parse_args(["--elf", "x.elf"] + list(args))


@pytest.fixture(autouse=True)
def clean_env(monkeypatch):
    monkeypatch.delenv("CORTRACE_OUT_DIR", raising=False)
    monkeypatch.delenv("CORTRACE_IFACE", raising=False)


@pytest.fixture(autouse=True)
def no_thread_resolution(monkeypatch):
    """The thread-name step has its own tests (test_osctl)."""
    monkeypatch.setattr(capture, "build_thread_map", lambda *_a: "map.txt")


def test_output_dir_must_be_given_explicitly(capsys):
    with pytest.raises(SystemExit) as e:
        parse("--iface", "eth0")
    assert e.value.code == 2
    assert "--out-dir is required" in capsys.readouterr().err


def test_output_dir_from_environment(monkeypatch, tmp_path):
    monkeypatch.setenv("CORTRACE_OUT_DIR", str(tmp_path / "o"))
    a = parse("--iface", "eth0")
    assert a.out_dir == str(tmp_path / "o")


def test_iface_needed_unless_raw_in(tmp_path, capsys):
    with pytest.raises(SystemExit):
        parse("--out-dir", str(tmp_path))
    assert "--iface" in capsys.readouterr().err
    a = parse("--out-dir", str(tmp_path), "--raw-in", "r.bin")
    assert a.iface is None


def test_defaults_and_unknown_options(tmp_path, capsys):
    a = parse("--out-dir", str(tmp_path), "--raw-in", "r")
    assert a.tag  # defaulted to a timestamp
    assert not a.notes and a.itm_port == 1
    with pytest.raises(SystemExit) as e:
        parse("--out-dir", str(tmp_path), "--raw-in", "r", "--fuse")
    assert e.value.code == 2
    assert "unrecognized arguments: --fuse" in capsys.readouterr().err


def args(tmp_path, *extra, **kw):
    a = parse("--out-dir", str(tmp_path), "--iface", "eth0", "--tag", "t", *extra)
    for key, value in kw.items():
        setattr(a, key, value)
    return a


@pytest.mark.parametrize(
    "base,expect",
    [("cycle", "--cycle-time"), ("etm", "--etm-time"), ("hybrid", "--hybrid-time")],
)
def test_decode_command_time_bases(tmp_path, base, expect):
    a = args(tmp_path, time_base=base, tsgen_hz=75e6, phase="1,0")
    cmd = capture.decode_command(a, "raw", "out.pf", "syms", "m.txt")
    assert expect in cmd and cmd[cmd.index("--perf") + 1] == "out.pf"
    assert cmd[cmd.index("--phase") + 1] == "1,0"
    assert cmd[cmd.index("--thread-map") + 1] == "m.txt"
    assert cmd[cmd.index("--trace-width") + 1] == "4"
    assert "--itm-note-out" not in cmd  # no notes unless asked


@pytest.mark.parametrize("base", ["etm", "hybrid"])
def test_decode_command_needs_tsgen_for_etm_clocks(tmp_path, base):
    with pytest.raises(SystemExit) as e:
        capture.decode_command(args(tmp_path, time_base=base), "r", "o", "s")
    assert "--tsgen-hz" in str(e.value)


def test_notes_ask_the_decoder_for_the_note_stream_and_the_switch_log(tmp_path):
    a = args(tmp_path, "--notes", "--itm-port", "0")
    cmd = capture.decode_command(a, "raw", "out.pf", "syms")
    assert cmd[cmd.index("--itm-note-port") + 1] == "0"
    assert "--itm-note-unwrap" in cmd
    assert cmd[cmd.index("--itm-note-out") + 1] == str(tmp_path / "notes_t.bin")
    assert "--runs-out" not in cmd  # the Threads track is the switch log


def test_notes_use_the_hybrid_time_base_with_a_default_tsgen(tmp_path):
    a = args(tmp_path, "--notes", time_base="cycle")
    cmd = capture.decode_command(a, "raw", "out.pf", "syms")
    assert "--hybrid-time" in cmd and "--cycle-time" not in cmd
    assert cmd[cmd.index("--tsgen-hz") + 1] == "75000000.0"
    given = args(tmp_path, "--notes", "--tsgen-hz", "100e6")
    cmd = capture.decode_command(given, "raw", "out.pf", "syms")
    assert cmd[cmd.index("--tsgen-hz") + 1] == "100000000.0"


def test_grab_sets_width_then_streams(monkeypatch, tmp_path):
    calls = []
    monkeypatch.setattr(capture.ctrl, "main", lambda a: calls.append(a) or 0)
    monkeypatch.setattr(
        capture,
        "run_cmd",
        lambda cmd, **_k: calls.append(cmd) or types.SimpleNamespace(returncode=0),
    )
    monkeypatch.setenv("CORTRACE_GRAB", "/opt/cortrace-grab")
    capture.grab(args(tmp_path, width=2, secs=3.0), "raw.bin")
    assert calls[0] == ["--iface", "eth0", "set-width", "2"]
    assert calls[1] == ["/opt/cortrace-grab", "eth0", "3.0", "raw.bin", "256", "512"]


@pytest.mark.parametrize("which", ["width", "grab"])
def test_grab_failures_abort(monkeypatch, tmp_path, which):
    monkeypatch.setattr(capture.ctrl, "main", lambda a: 1 if which == "width" else 0)
    monkeypatch.setattr(
        capture,
        "run_cmd",
        lambda cmd, **_k: types.SimpleNamespace(returncode=1),
    )
    with pytest.raises(SystemExit) as e:
        capture.grab(args(tmp_path, width=4), "raw.bin")
    assert ("set-width" if which == "width" else "cortrace-grab") in str(e.value)


def test_grab_can_keep_a_capture_that_lost_frames(monkeypatch, tmp_path, capsys):
    """cortrace-grab exits 1 after a seq gap but the file is written."""
    codes = iter([1, 1, 2])
    monkeypatch.setattr(
        capture,
        "run_cmd",
        lambda cmd, **_k: types.SimpleNamespace(returncode=next(codes)),
    )
    raw = tmp_path / "raw.bin"
    raw.write_bytes(b"\0")
    a = args(tmp_path)
    assert capture.grab(a, str(raw), allow_loss=True) is False
    assert "UDP frames were lost" in capsys.readouterr().err
    with pytest.raises(SystemExit):
        capture.grab(a, str(raw))  # plain capture: still an error
    with pytest.raises(SystemExit):
        capture.grab(a, str(raw), allow_loss=True)  # not a loss: an error


def fake_decode(ok=True, notes=None):
    """run_cmd stand-in: writes the outputs the real decoder would."""

    def run_cmd(cmd, **_k):
        if ok and "--perf" in cmd:
            open(cmd[cmd.index("--perf") + 1], "wb").close()
        if ok and notes is not None and "--itm-note-out" in cmd:
            with open(cmd[cmd.index("--itm-note-out") + 1], "wb") as f:
                f.write(notes)
        out = "stream health : lost-sync=0 -> clean\nchatter nobody needs\n"
        return types.SimpleNamespace(returncode=0 if ok else 1, stderr=out, stdout="")

    return run_cmd


def test_run_cmd_logs_the_command_and_runs_it(capsys):
    assert capture.run_cmd(["true"]).returncode == 0
    assert "$ true" in capsys.readouterr().err


def test_run_from_raw_file(monkeypatch, tmp_path, capsys):
    raw = tmp_path / "r.bin"
    raw.write_bytes(b"x")
    monkeypatch.setattr(capture, "run_cmd", fake_decode())
    monkeypatch.setattr(capture.subprocess, "run", lambda *a, **_k: None)
    result = capture.run(args(tmp_path / "out"), raw_in=str(raw))
    assert result == str(tmp_path / "out" / "hw_t.perfetto")
    err = capsys.readouterr().err
    assert "stream health" in err and "chatter" not in err  # only the key lines


def test_run_with_notes_reports_where_they_went(monkeypatch, tmp_path, capsys):
    raw = tmp_path / "r.bin"
    raw.write_bytes(b"x")
    monkeypatch.setattr(capture, "run_cmd", fake_decode(notes=b"\x10\x04"))
    monkeypatch.setattr(capture.subprocess, "run", lambda *a, **_k: None)
    result = capture.run(args(tmp_path / "out", "--notes"), raw_in=str(raw))
    assert result == str(tmp_path / "out" / "hw_t.perfetto")
    err = capsys.readouterr().err
    assert "notes_t.bin" in err and "hwruns" not in err and "warning" not in err


@pytest.mark.parametrize("notes", [None, b""])
def test_run_with_no_note_bytes_warns_and_keeps_the_hardware_trace(
    monkeypatch, tmp_path, capsys, notes
):
    raw = tmp_path / "r.bin"
    raw.write_bytes(b"x")
    monkeypatch.setattr(capture, "run_cmd", fake_decode(notes=notes))
    monkeypatch.setattr(capture.subprocess, "run", lambda *a, **_k: None)
    result = capture.run(
        args(tmp_path / "out", "--notes", "--itm-port", "0"), raw_in=str(raw)
    )
    assert result.endswith("hw_t.perfetto")
    assert "no note bytes on ITM port 0" in capsys.readouterr().err


def test_run_reports_decode_failure_and_missing_raw(monkeypatch, tmp_path):
    raw = tmp_path / "r.bin"
    raw.write_bytes(b"x")
    monkeypatch.setattr(capture, "run_cmd", fake_decode(ok=False))
    monkeypatch.setattr(capture.subprocess, "run", lambda *a, **_k: None)
    with pytest.raises(SystemExit) as e:
        capture.run(args(tmp_path / "o"), raw_in=str(raw))
    assert "decode failed" in str(e.value)
    with pytest.raises(SystemExit) as e:
        capture.run(args(tmp_path / "o"), raw_in=str(tmp_path / "missing"))
    assert "not found" in str(e.value)


def test_run_captures_when_no_raw_given(monkeypatch, tmp_path):
    grabbed = []
    monkeypatch.setattr(capture, "grab", lambda a, raw: grabbed.append(raw))
    monkeypatch.setattr(capture, "decode_hw", lambda a, raw: raw + ".pf")
    result = capture.run(args(tmp_path / "o"))
    assert grabbed == [str(tmp_path / "o" / "raw_t.bin")]
    assert result.endswith("raw_t.bin.pf")


def test_main_prints_result_and_opens(monkeypatch, tmp_path, capsys):
    opened = []
    monkeypatch.setattr(capture, "run", lambda a, raw_in=None: "/x/y.pf")
    monkeypatch.setattr(capture, "perfetto_open_main", lambda a: opened.append(a) or 0)
    base = ["--out-dir", str(tmp_path), "--elf", "e", "--raw-in", "r"]
    assert capture.main(base) == 0
    assert capsys.readouterr().out.strip() == "/x/y.pf" and not opened
    assert capture.main(base + ["--open"]) == 0
    assert opened == [["/x/y.pf", "--keep"]]
