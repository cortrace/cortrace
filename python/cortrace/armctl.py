"""`cortrace arm` -- arm the target's trace hardware through a running OpenOCD.

  cortrace arm --list
  cortrace arm --platform stm32h743 --elf fw.elf --probe-telnet 127.0.0.1:4444

The OS plugin that matches the ELF says which variable the DWT watches. Leave
the OpenOCD running afterwards: when the debugger disconnects, the OS may clear
the DWT configuration.
"""

import argparse
import sys

from . import osplugins, platdesc, trace_arm
from .elfinfo import ElfError, ElfInfo
from .osplugins import Watch
from .probe import OpenOcdTelnet, ProbeError


def log(msg):
    """Progress line on stderr."""
    print(f"[cortrace-arm] {msg}", file=sys.stderr)


def _dwt_events(text):
    names = [t.strip() for t in text.split(",") if t.strip()]
    if names == ["all"]:
        return tuple(trace_arm.DWT_EVENT_BITS)
    return tuple(names)


def build_parser():
    """Argument parser of `cortrace arm`."""
    ap = argparse.ArgumentParser(
        prog="cortrace arm",
        description="arm ETM, ITM and the DWT thread-switch trace over the probe",
    )
    ap.add_argument("--list", action="store_true", help="list the platforms")
    ap.add_argument("--platform", help="platform name or path of its .toml file")
    ap.add_argument(
        "--probe-telnet",
        metavar="HOST:PORT",
        help="telnet port of a running OpenOCD (usually 127.0.0.1:4444)",
    )
    ap.add_argument("--elf", help="firmware ELF (the OS plugin finds the variable)")
    ap.add_argument(
        "--os",
        default="auto",
        help="OS plugin name, 'auto' or 'none': no OS (bare metal), nothing is "
        "watched and only the ETM and ITM are armed (default: auto)",
    )
    ap.add_argument(
        "--watch-addr",
        type=lambda s: int(s, 0),
        action="append",
        default=[],
        metavar="ADDR",
        help="watch this address instead of asking the OS plugin (repeatable)",
    )
    ap.add_argument("--width", type=int, default=4, help="TPIU port width in bits")
    ap.add_argument(
        "--no-etm", action="store_true", help="arm only the ITM/DWT, keep the ETM off"
    )
    ap.add_argument(
        "--profile",
        choices=sorted(trace_arm.ETM_PROFILES),
        default="timing",
        help="ETM profile: timing (cycle counts + timestamps, for Perfetto) or "
        "coverage (branches only, about 4x fewer trace bytes, for `cortrace "
        "cover`; the Perfetto time bases need timing) (default: %(default)s)",
    )
    ap.add_argument(
        "--dwt-events",
        type=_dwt_events,
        default=(),
        metavar="LIST",
        help="DWT event counters to send as packets, comma separated: "
        f"{','.join(trace_arm.DWT_EVENT_BITS)} or all. Each packet stands for 256 "
        "events; `cortrace profile` places them in the functions. Default: none.",
    )
    return ap


def _watches(args):
    if args.watch_addr:
        return [Watch(f"0x{a:08x}", a) for a in args.watch_addr]
    if args.os == "none":
        # Bare metal: no current-task variable, so no DWT comparator. The ETM
        # and ITM are armed as usual.
        log("OS: none, no variable watched")
        return []
    if not args.elf:
        raise osplugins.PluginError("pass --elf or --watch-addr")
    elf = ElfInfo(args.elf)
    try:
        plugin = osplugins.select(args.os, elf)
        if plugin is None:
            raise osplugins.PluginError(
                "no OS plugin matches this ELF; pass --os NAME or --watch-addr"
            )
        log(f"OS: {plugin.name}")
        return plugin.watch(elf)
    finally:
        elf.close()


def _list():
    for name, path in sorted(platdesc.available().items()):
        print(f"{name}\t{path}")
    return 0


def main(argv=None):
    """Entry point of `cortrace arm`."""
    ap = build_parser()
    args = ap.parse_args(argv)
    if args.list:
        return _list()
    if not args.platform or not args.probe_telnet:
        ap.error("--platform and --probe-telnet are required")
    probe = None
    try:
        platform = platdesc.load(args.platform)
        watches = _watches(args)
        for item in watches:
            log(f"watching {item.symbol} at 0x{item.addr:08x}")
        probe = OpenOcdTelnet(args.probe_telnet)
        state = trace_arm.arm(
            probe,
            platform,
            watches,
            width=args.width,
            etm=not args.no_etm,
            profile=args.profile,
            dwt_events=args.dwt_events,
            log=log,
        )
    except (
        platdesc.PlatformError,
        osplugins.PluginError,
        ElfError,
        ProbeError,
        trace_arm.ArmError,
    ) as exc:
        print(f"cortrace arm: {exc}", file=sys.stderr)
        return 1
    finally:
        if probe is not None:
            probe.close()
    log(
        f"armed: DWT COMP0=0x{state['dwt_comp0']:08x} "
        f"FUNC0=0x{state['dwt_func0']:08x} ITM_TCR=0x{state['itm_tcr']:08x}"
    )
    return 0
