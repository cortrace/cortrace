"""`cortrace os` and the thread-name step of capture."""

import types

import pytest
from elffixture import FREERTOS_C, NUTTX_C, build

from cortrace import capture, osctl, osplugins
from cortrace.osplugins import ThreadInfo, threadmap


@pytest.fixture(autouse=True)
def isolate_user_plugins(monkeypatch, tmp_path):
    monkeypatch.setattr(osplugins, "USER_DIR", str(tmp_path / "no-plugins"))
    monkeypatch.delenv("CORTRACE_PLUGIN_PATH", raising=False)


@pytest.fixture(name="nuttx_path")
def nuttx_path_fixture(tmp_path):
    return build(tmp_path, "nuttx", NUTTX_C, "-g")


@pytest.fixture(name="rtos_path")
def rtos_path_fixture(tmp_path):
    return build(tmp_path, "rtos", FREERTOS_C, "-g")


def test_list_shows_every_plugin(capsys):
    assert osctl.main(["list"]) == 0
    out = capsys.readouterr().out
    assert out.startswith("freertos\t") and "\nnuttx\t" in out


def test_watch_prints_the_variable_and_its_address(capsys, nuttx_path):
    assert osctl.main(["watch", "--elf", nuttx_path]) == 0
    symbol, addr, size = capsys.readouterr().out.split()
    assert (symbol, size) == ("g_running_tasks", "4")
    assert addr.startswith("0x")


def test_watch_addr_only_is_script_friendly(capsys, rtos_path):
    assert osctl.main(["watch", "--elf", rtos_path, "--addr-only"]) == 0
    assert capsys.readouterr().out.strip().startswith("0x")


def test_watch_with_no_matching_os_fails(capsys, tmp_path):
    bare = build(tmp_path, "bare", "void main(void) {}", "-g")
    assert osctl.main(["watch", "--elf", bare]) == 1
    assert "no OS plugin matches" in capsys.readouterr().err


def test_resolve_writes_the_thread_map(tmp_path, rtos_path, monkeypatch):
    values = tmp_path / "v.txt"
    values.write_text("0x20000000\n")
    out = tmp_path / "map.txt"

    class FakeProbe:
        def __init__(self, hostport):
            assert hostport == "127.0.0.1:4444"
            self.closed = False

        def read_bytes(self, addr, size):
            data = b"worker\0".ljust(16, b"\0") if addr == 0x20000008 else None
            return data[:size] if data else None

        def close(self):
            self.closed = True

    created = []
    monkeypatch.setattr(
        osctl, "open_probe", lambda hp: created.append(FakeProbe(hp)) or created[-1]
    )
    argv = ["resolve", "--elf", rtos_path, "--values", str(values)]
    argv += ["--out", str(out), "--probe-telnet", "127.0.0.1:4444"]
    assert osctl.main(argv) == 0
    assert threadmap.read_map(str(out))[0x20000000].name == "worker"
    assert created[0].closed


def test_errors_are_reported_not_raised(capsys, tmp_path):
    bare = build(tmp_path, "bare", "void main(void) {}", "-g")
    argv = ["resolve", "--elf", bare, "--values", str(tmp_path / "v"), "--out", "m"]
    (tmp_path / "v").write_text("0x1\n")
    assert osctl.main(argv + ["--os", "nuttx"]) == 1  # no tcb_s in this ELF
    assert "cortrace os:" in capsys.readouterr().err


def test_resolve_without_a_plugin_keeps_only_the_extra_map(tmp_path):
    bare = build(tmp_path, "bare", "void main(void) {}", "-g")
    extra = {0x10: ThreadInfo(1, "x")}
    got = osctl.resolve_threads(bare, "auto", {0x10, 0x20}, extra=extra)
    assert got == extra
    assert osctl.resolve_threads(bare, "none", {0x10}) == {}


def test_extra_map_wins_over_the_plugin(rtos_path):
    extra = {0x20000000: ThreadInfo(9, "pinned")}
    got = osctl.resolve_threads(rtos_path, "freertos", {0x20000000}, extra=extra)
    assert got[0x20000000].name == "pinned"


# ---- capture ----------------------------------------------------------------
def capture_args(tmp_path, elf, *extra):
    a = capture.parse_args(
        [
            "--out-dir",
            str(tmp_path),
            "--raw-in",
            "r",
            "--elf",
            elf,
            "--tag",
            "t",
            *extra,
        ]
    )
    return a


def test_capture_options(tmp_path):
    a = capture_args(tmp_path, "e", "--nx-switch-stream", "2", "--os", "none")
    assert a.switch_stream == 2 and a.os == "none" and a.probe_telnet is None


def test_decode_command_passes_the_thread_map(tmp_path):
    a = capture_args(tmp_path, "e", "--time-base", "cycle")
    cmd = capture.decode_command(a, "raw", "out.pf", "syms", "map.txt")
    assert cmd[cmd.index("--thread-map") + 1] == "map.txt"
    assert "--switch-stream" in cmd and "--nx-tcbmap" not in cmd
    assert "--thread-map" not in capture.decode_command(a, "raw", "out.pf", "syms")


def test_build_thread_map_dumps_values_then_resolves(tmp_path, monkeypatch, nuttx_path):
    a = capture_args(tmp_path, nuttx_path, "--phase", "1,0")
    seen = []

    def fake_run(cmd, **_k):
        seen.append(cmd)
        out = cmd[cmd.index("--dump-switch-values") + 1]
        with open(out, "w", encoding="utf-8") as f:
            f.write("0x24000000\n")
        return types.SimpleNamespace(returncode=0)

    monkeypatch.setattr(capture, "run_cmd", fake_run)
    path = capture.build_thread_map(a, "raw", "syms")
    assert path == str(tmp_path / "threads_t.txt")
    assert "--phase" in seen[0] and "--perf" not in seen[0]
    assert not (tmp_path / "switches_t.txt").exists()  # intermediate removed
    assert not threadmap.read_map(path)  # nothing readable at that pointer


def test_build_thread_map_merges_the_legacy_map(tmp_path, monkeypatch, nuttx_path):
    legacy = tmp_path / "legacy.txt"
    legacy.write_text("0x24000000\t3\tmain_task\n")
    a = capture_args(tmp_path, nuttx_path, "--tcbmap", str(legacy))
    monkeypatch.setattr(
        capture,
        "run_cmd",
        lambda cmd, **_k: open(cmd[-1], "w", encoding="utf-8").close()
        or types.SimpleNamespace(returncode=0),
    )
    path = capture.build_thread_map(a, "raw", "syms")
    assert threadmap.read_map(path)[0x24000000] == ThreadInfo(3, "main_task")


def test_build_thread_map_reports_failures(tmp_path, monkeypatch, nuttx_path):
    a = capture_args(tmp_path, nuttx_path)
    monkeypatch.setattr(
        capture, "run_cmd", lambda *_a, **_k: types.SimpleNamespace(returncode=1)
    )
    with pytest.raises(SystemExit, match="thread switches"):
        capture.build_thread_map(a, "raw", "syms")

    def dump_ok(cmd, **_k):
        open(cmd[-1], "w", encoding="utf-8").close()
        return types.SimpleNamespace(returncode=0)

    monkeypatch.setattr(capture, "run_cmd", dump_ok)
    a.os = "bogus"
    with pytest.raises(SystemExit, match="thread names: unknown OS plugin"):
        capture.build_thread_map(a, "raw", "syms")
