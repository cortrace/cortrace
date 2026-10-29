"""Contract between the Python tools and the cortrace-decode binary.

capture builds cortrace-decode command lines. If a flag is renamed or dropped
on the C++ side, nothing in the Python unit tests would notice; this test runs
the real binary's --help and checks every flag we pass exists. It is skipped
when no binary is available (the Python-only CI job) and runs in the .deb
build job and on developer machines with a build tree.
"""

import os
import subprocess

import pytest

from cortrace import capture, cover
from cortrace._paths import find_binary

BINARY = find_binary("CORTRACE_DECODE", "cortrace-decode")
COV_BINARY = find_binary("CORTRACE_COV", "cortrace-cov")

pytestmark = pytest.mark.skipif(
    not (os.path.isfile(BINARY) and os.access(BINARY, os.X_OK)),
    reason="cortrace-decode not built",
)


@pytest.fixture(scope="module")
def help_text():
    out = subprocess.run([BINARY, "--help"], capture_output=True, text=True, check=True)
    return out.stdout + out.stderr


def flags(cmd):
    return {tok for tok in cmd if tok.startswith("--")}


def capture_command(tmp_path, base, *extra):
    a = capture.parse_args(
        ["--out-dir", str(tmp_path), "--elf", "e", "--raw-in", "r"]
        + ["--time-base", base, "--tsgen-hz", "75e6", "--phase", "1,0"]
        + list(extra)
    )
    return capture.decode_command(a, "raw", "out.pf", "syms.nm", "threads.txt")


def test_dump_switch_values_flags_exist(help_text, tmp_path):
    a = capture.parse_args(["--out-dir", str(tmp_path), "--elf", "e", "--raw-in", "r"])
    cmd = capture.base_command(a, "raw", "syms.nm") + ["--dump-switch-values", "v"]
    missing = [f for f in sorted(flags(cmd)) if f not in help_text]
    assert not missing, f"cortrace-decode no longer has: {missing}"


@pytest.mark.parametrize("base", ["cycle", "etm", "hybrid"])
def test_capture_flags_exist(help_text, tmp_path, base):
    cmd = capture_command(tmp_path, base)
    missing = [f for f in sorted(flags(cmd)) if f not in help_text]
    assert not missing, f"cortrace-decode no longer has: {missing}"


def test_capture_notes_flags_exist(help_text, tmp_path):
    """--notes depends on the note stream and the switch log outputs."""
    cmd = capture_command(tmp_path, "cycle", "--notes")
    missing = [f for f in sorted(flags(cmd)) if f not in help_text]
    assert not missing, f"cortrace-decode no longer has: {missing}"
    for flag in ("--itm-note-out", "--itm-note-unwrap"):
        assert flag in cmd


@pytest.mark.parametrize("source", ["--raw-in", "--etm-in"])
def test_cover_flags_exist(help_text, tmp_path, source):
    """cortrace cover run decodes with --cov-out / --cov-lag."""
    a = cover.parse_run_args(
        ["--out-dir", str(tmp_path), "--elf", "e", source, "r", "--phase", "1,0"]
    )
    cmd = cover.decode_command(a, "raw", "syms.nm", "obj.json")
    missing = [f for f in sorted(flags(cmd)) if f not in help_text]
    assert not missing, f"cortrace-decode no longer has: {missing}"
    assert "--cov-out" in cmd and "--cov-lag" in cmd


def test_version_flag_reports_the_version_file():
    """cortrace-decode --version is the VERSION file, optionally with a +local build label."""
    out = subprocess.run(
        [BINARY, "--version"], capture_output=True, text=True, check=True
    ).stdout.strip()
    version_file = os.path.join(os.path.dirname(__file__), "..", "..", "VERSION")
    with open(version_file, encoding="utf-8") as f:
        want = f.read().strip()
    assert out == want or out.startswith(want + "+"), f"{out!r} vs VERSION {want!r}"


@pytest.mark.skipif(
    not (os.path.isfile(COV_BINARY) and os.access(COV_BINARY, os.X_OK)),
    reason="cortrace-cov not built",
)
@pytest.mark.parametrize("source", ["--raw-in", "--iface"])
def test_cover_stream_flags_exist(tmp_path, source):
    """`cortrace cover stream` runs cortrace-cov with these options."""
    out = subprocess.run(
        [COV_BINARY, "--help"], capture_output=True, text=True, check=False
    )
    help_text = out.stdout + out.stderr
    a = cover.parse_stream_args(
        ["--out-dir", str(tmp_path), "--elf", "e", source, "x", "--secs", "5"]
        + ["--workers", "4", "--phase", "1,0"]
    )
    cmd = cover.stream_command(a, "obj.json")
    missing = [f for f in sorted(flags(cmd)) if f not in help_text]
    assert not missing, f"cortrace-cov no longer has: {missing}"
    assert "--cov-lag" in cmd and "--checkpoint-secs" in cmd
