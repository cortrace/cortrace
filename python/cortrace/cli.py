"""`cortrace` command line: one entry point for decode and capture.

This module only picks the sub-command (argparse sub-parsers) and hands the
rest of the command line to that command's own argparse parser, so every
command keeps its full option set and its own --help.
"""

import argparse
import os
import sys

from . import __version__, banner

# command -> one-line summary shown by `cortrace --help`
SUMMARIES = {
    "decode": "run cortrace-decode (ETM/DWT/ITM -> Perfetto), arguments passed through",
    "capture": "grab a raw capture from the FPGA streamer, decode it and open",
    "serve": 'Perfetto "Record" target: press Start in the UI to capture and view',
    "cover": "code coverage from the trace: run, report (text/json/lcov/cobertura), "
    "merge, diff",
    "tcbmap": "dump the live pid -> thread-name map through OpenOCD",
    "os": "OS plugins: list, find the variable to watch, resolve thread names",
    "arm": "arm ETM/ITM/DWT trace on the target through a running OpenOCD",
    "open": "open a Perfetto trace in ui.perfetto.dev",
    "fpga": "FPGA streamer tools: ctrl (CSR writes), net (find the link), health",
    "version": "print the version",
    "help": "show this help",
}

# fpga sub-tool -> module under cortrace/
FPGA_TOOLS = {
    "ctrl": "fpga.ctrl",
    "net": "fpga.net",
    "health": "fpga.health",
}

# Commands that run for a while and are driven by a person at a terminal; the
# short helpers (version, tcbmap, open, fpga, decode) stay quiet.
BANNER_COMMANDS = ("capture", "serve")


def _decode(args):
    from ._paths import find_binary

    binary = find_binary("CORTRACE_DECODE", "cortrace-decode")
    try:
        os.execv(binary, [binary] + args)
    except OSError as exc:
        print(f"cortrace: cannot run {binary}: {exc}", file=sys.stderr)
        return 127
    return 0  # not reached


def _lazy(module, attr="main"):
    def run(args):
        import importlib

        return getattr(importlib.import_module(f"cortrace.{module}"), attr)(args)

    return run


# command -> handler(list of remaining arguments) -> exit code
COMMANDS = {
    "decode": _decode,
    "capture": _lazy("capture"),
    "serve": _lazy("serve"),
    "cover": _lazy("cover"),
    "tcbmap": _lazy("tcbmap"),
    "os": _lazy("osctl"),
    "arm": _lazy("armctl"),
    "open": _lazy("perfetto_open"),
}


def _banner_option(default):
    """A parent parser carrying --no-banner.

    The main parser uses default=False; the copies on the sub-parsers use
    SUPPRESS so that they only override the value when the flag is present
    (otherwise a sub-parser's default would reset a flag given before the
    command name).
    """
    parent = argparse.ArgumentParser(add_help=False)
    parent.add_argument(
        "--no-banner",
        action="store_true",
        default=default,
        help="do not print the logo (also: CORTRACE_NO_BANNER=1)",
    )
    return parent


def build_parser():
    parser = argparse.ArgumentParser(
        prog="cortrace",
        description="Cortex-M ETM/DWT/ITM trace: capture, decode and view.",
        epilog="Run `cortrace <command> --help` for the options of a command. "
        "The logo is printed only on a terminal.",
        parents=[_banner_option(False)],
        add_help=False,  # -h is handled in main() so the banner can come first
        allow_abbrev=False,  # never swallow an option meant for the command
    )
    parser.add_argument("-h", "--help", action="store_true", help="show this help")
    parser.add_argument(
        "-V", "--version", action="version", version=f"cortrace {__version__}"
    )
    commands = parser.add_subparsers(
        dest="command", metavar="<command>", title="commands"
    )
    for name, summary in SUMMARIES.items():
        sub = commands.add_parser(
            name,
            help=summary,
            # the command parses its own --help; this parser only routes
            add_help=False,
            allow_abbrev=False,
            parents=[_banner_option(argparse.SUPPRESS)],
        )
        if name == "fpga":
            tools = sub.add_subparsers(dest="tool", metavar="<tool>", required=True)
            for tool in FPGA_TOOLS:
                tools.add_parser(tool, add_help=False, allow_abbrev=False)
    return parser


def main(argv=None):
    parser = build_parser()
    ns, rest = parser.parse_known_args(sys.argv[1:] if argv is None else argv)

    if ns.command in (None, "help") or ns.help:
        if rest and ns.command is None:
            parser.error(f"unrecognized arguments: {' '.join(rest)}")
        if not ns.no_banner:
            banner.show()
        parser.print_help()
        return 0 if (ns.help or ns.command == "help") else 2
    if ns.command == "version":
        print(f"cortrace {__version__}")
        return 0

    if ns.command in BANNER_COMMANDS and not ns.no_banner:
        banner.show()
    if ns.command == "fpga":
        return _lazy(FPGA_TOOLS[ns.tool])(rest) or 0
    return COMMANDS[ns.command](rest) or 0
