"""Platform descriptions: where the CoreSight blocks of a chip are and what the
chip needs before they can trace.

A platform is a TOML file (see platforms/stm32h743.toml). The part every Armv7-M
chip shares -- how the blocks are armed -- lives in trace_arm.py; the file only
holds addresses, ids, default clocks and the few chip-specific register steps
(`[[on_trace_start]]`).
"""

import os
import sys

from ._paths import _REPO

if sys.version_info >= (3, 11):
    import tomllib
else:  # pragma: no cover - python 3.10 (the deb depends on python3-tomli)
    import tomli as tomllib

SEARCH_ENV = "CORTRACE_PLATFORM_PATH"
USER_DIR = os.path.join(os.path.expanduser("~"), ".config", "cortrace", "platforms")
SHARE_DIR = "/usr/share/cortrace/platforms"
SOURCE_DIR = os.path.join(_REPO, "platforms")

BLOCKS = ("etm", "dwt", "itm", "tpiu")  # `tsgen` is optional: not every chip has one
# Between the ETM/ITM and the TPIU: funnels and ETFs. A chip with one funnel and
# one ETF names them (`cstf`, `etf`); a chip with more lists the stages in
# `[[coresight.path]]`.
LEGACY_PATH = ("cstf", "etf")
STAGE_KINDS = ("funnel", "etf")
OPS = {"write32": ("addr", "value"), "rmw32": ("addr",), "poll32": ("addr", "mask")}


class PlatformError(Exception):
    """The platform file is missing or malformed."""


def search_dirs():
    """Directories searched for platform files, first match wins."""
    extra = [d for d in os.environ.get(SEARCH_ENV, "").split(os.pathsep) if d]
    return extra + [USER_DIR, SHARE_DIR, SOURCE_DIR]


def available():
    """{name: path} of every platform file found."""
    found = {}
    for directory in reversed(search_dirs()):
        if not os.path.isdir(directory):
            continue
        for entry in sorted(os.listdir(directory)):
            if entry.endswith(".toml"):
                found[entry[: -len(".toml")]] = os.path.join(directory, entry)
    return found


def _check_ops(path, ops):
    for i, step in enumerate(ops, start=1):
        op = step.get("op")
        if op not in OPS:
            raise PlatformError(
                f"{path}: on_trace_start #{i}: unknown op {op!r} "
                f"(use {', '.join(OPS)})"
            )
        missing = [k for k in OPS[op] if k not in step]
        if missing:
            raise PlatformError(
                f"{path}: on_trace_start #{i} ({op}) needs {', '.join(missing)}"
            )


def _stages(path, blocks):
    """The trace path as a list of stages, in the order they are programmed.

    `[[coresight.path]]` entries have `kind` ("funnel" or "etf") and `base`; a
    funnel may also have `ports` (bit mask of the inputs to enable, default
    0x3) and `itm_priority` (give the ITM input priority, default true). Without
    a `path`, the funnel `cstf` and the ETF `etf` make the two stages. An empty
    `path = []` means the ETM and the ITM feed the TPIU directly (Cortex-M4 parts
    such as the STM32F4).
    """
    if "path" not in blocks:
        return [
            {
                "kind": "funnel",
                "base": blocks["cstf"],
                "ports": 0x3,
                "itm_priority": True,
            },
            {"kind": "etf", "base": blocks["etf"]},
        ]
    stages = []
    for i, stage in enumerate(blocks["path"] or [], start=1):
        if stage.get("kind") not in STAGE_KINDS or "base" not in stage:
            raise PlatformError(
                f"{path}: coresight.path #{i} needs kind "
                f"({' or '.join(STAGE_KINDS)}) and base"
            )
        stages.append(dict(stage))
        if stage["kind"] == "funnel":
            stages[-1].setdefault("ports", 0x3)
            stages[-1].setdefault("itm_priority", True)
    return stages


def load(name_or_path):
    """Load a platform by name or by path to its .toml file."""
    path = name_or_path
    if not name_or_path.endswith(".toml"):
        path = available().get(name_or_path)
        if path is None:
            known = ", ".join(sorted(available())) or "none found"
            raise PlatformError(
                f"unknown platform '{name_or_path}' (available: {known})"
            )
    try:
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except OSError as exc:
        raise PlatformError(f"cannot read {path}: {exc}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise PlatformError(f"{path}: {exc}") from exc
    blocks = data.get("coresight", {})
    missing = [b for b in BLOCKS if b not in blocks]
    if "path" not in blocks:
        missing += [b for b in LEGACY_PATH if b not in blocks]
    if missing:
        raise PlatformError(f"{path}: [coresight] lacks {', '.join(missing)}")
    blocks["path"] = _stages(path, blocks)
    trace = data.setdefault("trace", {})
    trace.setdefault("etm_trace_id", 2)
    trace.setdefault("itm_trace_id", 1)
    trace.setdefault("max_width", 4)
    trace.setdefault("default_width", trace["max_width"])
    trace.setdefault(
        "etm_version", 4
    )  # 3: ETMv3.5 of a Cortex-M3/M4, 4: ETMv4 of an M7
    if not 1 <= trace["default_width"] <= trace["max_width"]:
        raise PlatformError(f"{path}: trace.default_width must be within 1..max_width")
    if trace["etm_version"] not in (3, 4):
        raise PlatformError(f"{path}: trace.etm_version must be 3 or 4")
    data.setdefault("clocks", {})
    data.setdefault("on_trace_start", [])
    _check_ops(path, data["on_trace_start"])
    data["path"] = path
    return data
