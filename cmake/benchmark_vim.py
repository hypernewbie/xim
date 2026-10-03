#!/usr/bin/env python3
"""Measure repeatable startup, open, scroll, and edit latency for Vim binaries."""

import argparse
import fcntl
import json
import os
import pty
import select
import signal
import statistics
import struct
import subprocess
import tempfile
import termios
import time
from pathlib import Path


def parse_binaries(specs, minimum=2):
    binaries = []
    labels = set()
    for spec in specs:
        label, separator, name = spec.partition("=")
        if not separator or not label or not name:
            raise ValueError(f"invalid --binary value: {spec!r}; expected NAME=PATH")
        path = Path(name).resolve(strict=True)
        if not path.is_file() or not os.access(path, os.X_OK):
            raise ValueError(f"not an executable file: {path}")
        if label in labels:
            raise ValueError(f"duplicate binary label: {label}")
        labels.add(label)
        binaries.append((label, path))
    if len(binaries) < minimum:
        raise ValueError(f"provide at least {minimum} --binary NAME=PATH values")
    return binaries


def make_environment(runtime, home):
    env = os.environ.copy()
    env.update(
        {
            "VIMRUNTIME": str(runtime),
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "TERM": "xterm-256color",
            "LC_ALL": "C",
        }
    )
    env.pop("VIM", None)
    return env


def make_fixture(path, line_count):
    with path.open("w", encoding="ascii", newline="\n") as fixture:
        for number in range(line_count):
            fixture.write(f"XIM-BENCH-LINE-{number:06d} deterministic text for scrolling\n")


def measure_startup(binary, env):
    command = [
        str(binary),
        "-u",
        "NONE",
        "-U",
        "NONE",
        "-i",
        "NONE",
        "-n",
        "-N",
        "-X",
        "--noplugin",
        "-es",
        "-c",
        "qa!",
    ]
    start = time.perf_counter_ns()
    result = subprocess.run(
        command,
        env=env,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        timeout=10,
        check=False,
    )
    elapsed = time.perf_counter_ns() - start
    if result.returncode != 0:
        raise RuntimeError(
            f"startup command failed for {binary} ({result.returncode}): "
            f"{result.stderr.decode(errors='replace')}"
        )
    return elapsed / 1_000_000


def wait_for_output(master, process, marker, timeout):
    output = bytearray()
    deadline = time.monotonic() + timeout
    while marker not in output:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            tail = bytes(output[-512:]).decode("ascii", errors="replace")
            raise TimeoutError(
                f"Vim did not display marker {marker!r} within {timeout}s; "
                f"recent terminal output: {tail!r}"
            )
        if process.poll() is not None:
            raise RuntimeError(f"Vim exited with status {process.returncode} before displaying {marker!r}")
        ready, _, _ = select.select([master], [], [], min(remaining, 0.1))
        if not ready:
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError as error:
            raise RuntimeError(f"failed to read Vim's terminal output: {error}") from error
        if not chunk:
            raise RuntimeError(f"Vim closed its terminal before displaying {marker!r}")
        output.extend(chunk)
        if len(output) > 8 * 1024 * 1024:
            del output[: 4 * 1024 * 1024]


def measure_interactive(binary, env, fixture, line_count, rows, columns, self_test=False):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
    command = [
        str(binary),
        "-u",
        "NONE",
        "-U",
        "NONE",
        "-i",
        "NONE",
        "-n",
        "-N",
        "-X",
        "--noplugin",
        str(fixture),
    ]
    process = None
    try:
        start = time.perf_counter_ns()
        process = subprocess.Popen(
            command,
            env=env,
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
            start_new_session=True,
        )
        os.close(slave)
        slave = -1

        first_line = b"XIM-BENCH-LINE-000000"
        wait_for_output(master, process, first_line, 10)
        first_paint = (time.perf_counter_ns() - start) / 1_000_000

        # Markers are assembled by Vim, never present in the submitted command.
        # Validate buffer contents as well as reaching command dispatch/redraw.
        os.write(master, b"iREADY\x1b:redraw!|if getline(1) =~ '^READYXIM-'|echo 'XIM_' . 'READY_DONE'|endif\r")
        wait_for_output(master, process, b"XIM_READY_DONE", 10)
        ready_edit = (time.perf_counter_ns() - start) / 1_000_000

        if self_test:
            delayed_start = time.perf_counter_ns()
            os.write(master, b":sleep 1000m|redraw!|echo 'XIM_' . 'DELAY_DONE'\r")
            wait_for_output(master, process, b"XIM_DELAY_DONE", 10)
            elapsed = (time.perf_counter_ns() - delayed_start) / 1_000_000
            if elapsed < 1000:
                raise AssertionError(f"completion accepted before delay: {elapsed:.3f} ms")

        scroll_marker = f"{line_count} XIM_SCROLL_DONE".encode("ascii")
        scroll_start = time.perf_counter_ns()
        os.write(master, b"G:redraw!|echo line('.') . ' XIM_' . 'SCROLL_DONE'\r")
        wait_for_output(master, process, scroll_marker, 10)
        scroll = (time.perf_counter_ns() - scroll_start) / 1_000_000

        edit_start = time.perf_counter_ns()
        os.write(master, b"iXIM_BENCH_EDIT\x1b:redraw!|if getline('.') =~ '^XIM_BENCH_EDITXIM-'|echo 'XIM_' . 'EDIT_DONE'|endif\r")
        wait_for_output(master, process, b"XIM_EDIT_DONE", 10)
        edit = (time.perf_counter_ns() - edit_start) / 1_000_000

        os.write(master, b":qa!\r")
        process.wait(timeout=5)
        if process.returncode != 0:
            raise RuntimeError(f"Vim exited with status {process.returncode}")
        return first_paint, ready_edit, scroll, edit
    finally:
        if slave >= 0:
            os.close(slave)
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        os.close(master)


def summarize(samples):
    ordered = sorted(samples)
    p90 = ordered[max(0, (9 * len(ordered) + 9) // 10 - 1)]
    return {
        "samples": len(samples),
        "median_ms": round(statistics.median(samples), 3),
        "p90_ms": round(p90, 3),
        "min_ms": round(ordered[0], 3),
        "max_ms": round(ordered[-1], 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        action="append",
        required=True,
        metavar="NAME=PATH",
        help="Vim executable to measure; pass at least two to compare",
    )
    parser.add_argument("--runtime", required=True, type=Path, help="runtime/ directory shared by all binaries")
    parser.add_argument("--runs", type=int, default=15, help="samples per binary (default: 15)")
    parser.add_argument("--lines", type=int, default=10000, help="fixture line count (default: 10000)")
    parser.add_argument("--rows", type=int, default=24, help="PTY height (default: 24)")
    parser.add_argument("--columns", type=int, default=100, help="PTY width (default: 100)")
    parser.add_argument("--output", type=Path, help="optional JSON output path")
    parser.add_argument("--self-test", action="store_true", help="verify completion rejects echoed input using a one-second delay")
    args = parser.parse_args()

    try:
        binaries = parse_binaries(args.binary, minimum=1 if args.self_test else 2)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    runtime = args.runtime.resolve(strict=True)
    if not runtime.is_dir():
        parser.error(f"runtime path is not a directory: {runtime}")
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.lines <= args.rows or args.rows < 5 or args.columns < 40:
        parser.error("use --lines greater than --rows, at least 5 rows, and at least 40 columns")

    results = {
        label: {name: [] for name in ("startup_exit", "first_paint", "ready_edit", "scroll_to_end", "insert_visible")}
        for label, _ in binaries
    }
    with tempfile.TemporaryDirectory(prefix="xim-bench-") as temporary:
        temporary_path = Path(temporary)
        fixture = temporary_path / "scroll.txt"
        make_fixture(fixture, args.lines)
        for sample in range(args.runs):
            sample_binaries = binaries if sample % 2 == 0 else reversed(binaries)
            for label, binary in sample_binaries:
                home = temporary_path / f"home-{sample}-{label}"
                home.mkdir()
                env = make_environment(runtime, home)
                results[label]["startup_exit"].append(measure_startup(binary, env))
                painted, ready, scrolled, edited = measure_interactive(
                    binary, env, fixture, args.lines, args.rows, args.columns, args.self_test
                )
                results[label]["first_paint"].append(painted)
                results[label]["ready_edit"].append(ready)
                results[label]["scroll_to_end"].append(scrolled)
                results[label]["insert_visible"].append(edited)

    report = {
        "runtime": str(runtime),
        "runs_per_binary": args.runs,
        "fixture_lines": args.lines,
        "terminal": {"rows": args.rows, "columns": args.columns, "term": "xterm-256color"},
        "configuration": "-u NONE -U NONE -i NONE -n -N -X --noplugin",
        "results": {
            label: {name: summarize(samples) for name, samples in workloads.items()}
            for label, workloads in results.items()
        },
    }
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
