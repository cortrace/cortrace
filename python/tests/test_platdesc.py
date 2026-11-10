"""Platform files: lookup, validation."""

import pytest

from cortrace import platdesc

MINIMAL = """
[coresight]
etm = 0x1000
dwt = 0x2000
itm = 0x3000
cstf = 0x4000
etf = 0x5000
tpiu = 0x6000
tsgen = 0x7000
"""


@pytest.fixture(autouse=True)
def isolate(monkeypatch, tmp_path):
    monkeypatch.setattr(platdesc, "USER_DIR", str(tmp_path / "no-user-dir"))
    monkeypatch.delenv(platdesc.SEARCH_ENV, raising=False)


def write(tmp_path, text, name="chip.toml"):
    path = tmp_path / name
    path.write_text(text)
    return str(path)


def test_the_bundled_platform_loads_by_name():
    plat = platdesc.load("stm32h743")
    assert plat["coresight"]["tpiu"] == 0x5C015000
    assert plat["trace"] == {
        "etm_trace_id": 2,
        "itm_trace_id": 1,
        "max_width": 4,
        "default_width": 4,
        "etm_version": 4,
    }
    assert plat["clocks"]["tsgen_hz"] == 75000000
    assert plat["on_trace_start"][0]["op"] == "rmw32"


def test_defaults_fill_in_the_optional_sections(tmp_path):
    plat = platdesc.load(write(tmp_path, MINIMAL))
    assert plat["trace"]["etm_trace_id"] == 2 and plat["trace"]["max_width"] == 4
    assert plat["clocks"] == {} and plat["on_trace_start"] == []


def test_a_search_path_directory_adds_and_overrides_platforms(monkeypatch, tmp_path):
    write(tmp_path, MINIMAL, "mychip.toml")
    write(tmp_path, MINIMAL.replace("0x6000", "0x9999"), "stm32h743.toml")
    monkeypatch.setenv(platdesc.SEARCH_ENV, str(tmp_path))
    names = platdesc.available()
    assert "mychip" in names and names["stm32h743"].startswith(str(tmp_path))
    assert platdesc.load("stm32h743")["coresight"]["tpiu"] == 0x9999


def test_unknown_platform_lists_the_choices():
    with pytest.raises(platdesc.PlatformError, match="unknown platform 'x'.*stm32h743"):
        platdesc.load("x")


def test_unreadable_and_malformed_files(tmp_path):
    with pytest.raises(platdesc.PlatformError, match="cannot read"):
        platdesc.load(str(tmp_path / "missing.toml"))
    with pytest.raises(platdesc.PlatformError, match="chip.toml"):
        platdesc.load(write(tmp_path, "this is = = not toml"))


def test_every_block_must_have_an_address(tmp_path):
    with pytest.raises(platdesc.PlatformError, match=r"lacks tpiu, etf"):
        platdesc.load(
            write(tmp_path, MINIMAL.replace("etf = 0x5000\ntpiu = 0x6000\n", ""))
        )


def test_one_funnel_and_one_etf_make_the_default_path(tmp_path):
    path = platdesc.load(write(tmp_path, MINIMAL))["coresight"]["path"]
    assert path == [
        {"kind": "funnel", "base": 0x4000, "ports": 0x3, "itm_priority": True},
        {"kind": "etf", "base": 0x5000},
    ]


PATHS = """
[[coresight.path]]
kind = "etf"
base = 0x5000
[[coresight.path]]
kind = "funnel"
base = 0x4000
ports = 0x1
itm_priority = false
[[coresight.path]]
kind = "funnel"
base = 0x4800
"""
NO_CHAIN = "".join(
    line
    for line in MINIMAL.splitlines(keepends=True)
    if not line.startswith(("cstf", "etf"))
)


def test_a_path_replaces_cstf_and_etf(tmp_path):
    path = platdesc.load(write(tmp_path, NO_CHAIN + PATHS))["coresight"]["path"]
    assert [s["kind"] for s in path] == ["etf", "funnel", "funnel"]
    assert path[1] == {
        "kind": "funnel",
        "base": 0x4000,
        "ports": 1,
        "itm_priority": False,
    }
    assert path[2]["ports"] == 0x3 and path[2]["itm_priority"] is True  # defaults


@pytest.mark.parametrize(
    "stage",
    ['kind = "bridge"\nbase = 1\n', 'kind = "etf"\n', "base = 1\n"],
)
def test_a_bad_stage_is_rejected(tmp_path, stage):
    text = NO_CHAIN + "[[coresight.path]]\n" + stage
    with pytest.raises(platdesc.PlatformError, match=r"path #1 needs kind"):
        platdesc.load(write(tmp_path, text))


def test_an_empty_path_sends_the_trace_straight_to_the_tpiu(tmp_path):
    text = NO_CHAIN.replace("[coresight]\n", "[coresight]\npath = []\n")
    assert platdesc.load(write(tmp_path, text))["coresight"]["path"] == []


def test_the_timestamp_generator_is_optional(tmp_path):
    text = MINIMAL.replace("tsgen = 0x7000\n", "")
    assert "tsgen" not in platdesc.load(write(tmp_path, text))["coresight"]


@pytest.mark.parametrize("version", [2, 5])
def test_the_etm_version_is_3_or_4(tmp_path, version):
    text = MINIMAL + f"[trace]\netm_version = {version}\n"
    with pytest.raises(platdesc.PlatformError, match="etm_version must be 3 or 4"):
        platdesc.load(write(tmp_path, text))


@pytest.mark.parametrize("width", [0, 5])
def test_the_default_width_is_within_the_maximum(tmp_path, width):
    text = MINIMAL + f"[trace]\ndefault_width = {width}\n"
    with pytest.raises(platdesc.PlatformError, match="default_width"):
        platdesc.load(write(tmp_path, text))


def test_steps_are_checked(tmp_path):
    bad_op = MINIMAL + '[[on_trace_start]]\nop = "frob"\n'
    with pytest.raises(platdesc.PlatformError, match="#1: unknown op 'frob'"):
        platdesc.load(write(tmp_path, bad_op))
    no_addr = MINIMAL + '[[on_trace_start]]\nop = "write32"\naddr = 4\n'
    with pytest.raises(platdesc.PlatformError, match=r"\(write32\) needs value"):
        platdesc.load(write(tmp_path, no_addr))
