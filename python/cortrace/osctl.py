"""`cortrace os` -- OS plugins: list them, find the variable to watch, and turn
the pointers a capture saw into a thread map.

  cortrace os list
  cortrace os watch   --elf fw.elf [--os NAME]
  cortrace os resolve --elf fw.elf --values v.txt --out map.txt
                      [--os NAME] [--probe-telnet HOST:PORT]
"""

import argparse
import sys

from . import osplugins
from .elfinfo import ElfError, ElfInfo
from .osplugins import threadmap
from .probe import ProbeError, open_probe


def log(msg):
    """Progress line on stderr."""
    print(f"[cortrace-os] {msg}", file=sys.stderr)


def resolve_threads(elf_path, os_name, values, probe_hostport=None, extra=None):
    """{pointer: ThreadInfo} for `values`, or {} when there is no OS plugin.

    `extra` is a map that wins over the plugin's answers (for instance one
    dumped from the live target by `cortrace tcbmap`).
    """
    elf = ElfInfo(elf_path)
    probe = None
    try:
        plugin = osplugins.select(os_name, elf)
        if plugin is None:
            log("no OS plugin: threads are shown as raw pointers")
            return dict(extra or {})
        if probe_hostport:
            probe = open_probe(probe_hostport)
        threads = plugin.resolve(values, osplugins.Memory(elf, probe), elf)
        log(f"{plugin.name}: resolved {len(threads)} of {len(values)} pointers")
        threads.update(extra or {})
        return threads
    finally:
        if probe is not None:
            probe.close()
        elf.close()


def _add_selection(ap):
    ap.add_argument("--elf", required=True, help="firmware ELF")
    ap.add_argument(
        "--os",
        default="auto",
        help="OS plugin name, 'auto' (detect from the ELF) or 'none' (default: auto)",
    )


def build_parser():
    """Argument parser of `cortrace os`."""
    ap = argparse.ArgumentParser(
        prog="cortrace os", description="OS plugins for the thread overlay"
    )
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("list", help="list the available OS plugins")
    watch = sub.add_parser("watch", help="print the variable the DWT must watch")
    _add_selection(watch)
    watch.add_argument(
        "--addr-only", action="store_true", help="print only the first address"
    )
    res = sub.add_parser("resolve", help="build a thread map from captured pointers")
    _add_selection(res)
    res.add_argument(
        "--values", required=True, help="cortrace-decode --dump-switch-values file"
    )
    res.add_argument("--out", required=True, help="thread map to write")
    res.add_argument(
        "--probe-telnet",
        default=None,
        metavar="[gdb:]HOST:PORT",
        help="read live target memory through a running OpenOCD, or with the "
        "prefix gdb: a P&E GDB server (needed for TCBs that are not in the ELF "
        "image)",
    )
    return ap


def _list():
    for name, plugin in sorted(osplugins.available().items()):
        doc = (type(plugin).__doc__ or "").strip().splitlines()
        print(f"{name}\t{doc[0] if doc else ''}")
    return 0


def _watch(args):
    elf = ElfInfo(args.elf)
    try:
        plugin = osplugins.select(args.os, elf)
        if plugin is None:
            log("no OS plugin matches this ELF")
            return 1
        watches = plugin.watch(elf)
    finally:
        elf.close()
    if args.addr_only:
        print(f"0x{watches[0].addr:08x}")
        return 0
    for item in watches:
        print(f"{item.symbol}\t0x{item.addr:08x}\t{item.size}")
    return 0


def _resolve(args):
    values = threadmap.read_values(args.values)
    threads = resolve_threads(args.elf, args.os, values, args.probe_telnet)
    threadmap.write_map(args.out, threads)
    log(f"wrote {len(threads)} threads -> {args.out}")
    return 0


def main(argv=None):
    """Entry point of `cortrace os`."""
    args = build_parser().parse_args(argv)
    try:
        if args.action == "list":
            return _list()
        if args.action == "watch":
            return _watch(args)
        return _resolve(args)
    except (osplugins.PluginError, ElfError, ProbeError) as exc:
        print(f"cortrace os: {exc}", file=sys.stderr)
        return 1
