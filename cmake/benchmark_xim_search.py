#!/usr/bin/env python3
"""Latency for the native find, replace and project-search operations.

Serial runs only. Each phase uses an engine-observed completion so the timer
covers the real operation, not an echo. The fixture is generated once and the
project phase warms the index before the warm samples.
"""
import argparse
import json
import os
import select
import statistics
import tempfile
import time
from pathlib import Path

from test_xim import Session


def wait_fast(session, predicate, timeout=10):
    """Poll the terminal with a short select so the timer sees the real
    completion instead of the 100 ms drain interval."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select([session.master], [], [], 0.005)
        if ready:
            try:
                session.screen.feed(os.read(session.master, 65536))
            except OSError:
                break
        if predicate():
            return True
    return predicate()


def percentile(samples, fraction):
    ordered = sorted(samples)
    index = min(len(ordered) - 1, int(len(ordered) * fraction))
    return ordered[index]


def summary(samples):
    return {
        "median": round(statistics.median(samples), 3),
        "p90": round(percentile(samples, 0.9), 3),
        "min": round(min(samples), 3),
        "max": round(max(samples), 3),
    }


def build_large_file(directory, lines=100000, every=100):
    path = directory / "large.txt"
    with path.open("w") as stream:
        for i in range(lines):
            if i % every == 0:
                stream.write(f"needle line {i}\n")
            else:
                stream.write(f"filler line {i} with some text\n")
    return path


def build_project(directory, files=10000):
    directory.mkdir(parents=True, exist_ok=True)
    for i in range(files):
        group = directory / f"group{i % 200}"
        group.mkdir(exist_ok=True)
        marker = "needle here" if i % 50 == 0 else "only filler"
        (group / f"file{i}.txt").write_text(f"{marker} number {i}\n")


def measure_find(binary, directory, query, expected, runs):
    session = Session(binary, directory, ("large.txt",))
    samples = []
    marker = f"Find: {query}".encode()
    count = f"{expected} matches".encode()
    try:
        for _ in range(runs):
            session.send(b"\x06")  # Ctrl-F
            session.wait(b"Find:")
            start = time.monotonic()
            session.send(query)
            wait_fast(session, lambda: marker in session.screen.text()
                    and count in session.screen.text())
            samples.append((time.monotonic() - start) * 1000)
            session.send(b"\x1b")
            session.drain(.2)
    finally:
        session.close()
    return samples


def measure_replace(binary, directory, query, replacement, runs):
    session = Session(binary, directory, ("large.txt",))
    samples = []
    try:
        for _ in range(runs):
            session.send(b"\x1b")
            session.send(b"\x08")  # Ctrl-H
            session.wait(b"Replace:")
            session.send(query)
            wait_fast(session, lambda: b"1000 matches" in session.screen.text())
            session.send(b"\t")
            session.send(replacement)
            start = time.monotonic()
            session.send(b"\r")
            wait_fast(session, lambda: b"0 matches" in session.screen.text())
            samples.append((time.monotonic() - start) * 1000)
            session.send(b"\x1b")
            session.send(b"\x1a")  # undo the replace-all
            session.drain(.3)
    finally:
        session.close()
    return samples


def measure_project(binary, directory, query, runs):
    session = Session(binary, directory)
    samples = []
    cold = None
    try:
        start = time.monotonic()
        session.send(b"\x1bOP")
        session.send("Search project")
        session.send(b"\r")
        session.wait(b"Search:")
        session.send(query)
        wait_fast(session, lambda: b"needle here" in session.screen.text(), timeout=60)
        cold = (time.monotonic() - start) * 1000
        session.send(b"\x1b")
        session.drain(.3)
        for _ in range(runs):
            session.send(b"\x1bOP")
            session.send("Search project")
            session.send(b"\r")
            session.wait(b"Search:")
            start = time.monotonic()
            session.send(query)
            wait_fast(session, lambda: b"needle here" in session.screen.text(), timeout=30)
            samples.append((time.monotonic() - start) * 1000)
            session.send(b"\x1b")
            session.drain(.3)
    finally:
        session.close()
    return samples, cold


def measure_idle(binary, directory, seconds=1.0):
    session = Session(binary, directory)
    try:
        session.send(b"\x1bOP")
        session.send("Search project")
        session.send(b"\r")
        session.send("needle")
        session.drain(1.5)
        pid = session.process.pid
        ticks_before = _cpu_ticks(pid)
        bytes_out = len(session.drain(seconds))
        ticks_after = _cpu_ticks(pid)
        return {"bytes": bytes_out, "ticks": ticks_after - ticks_before}
    finally:
        session.close()


def _cpu_ticks(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split()
        return int(fields[13]) + int(fields[14])
    except OSError:
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build/dev/src/xim")
    parser.add_argument("--work", default="build/plan5/perf")
    parser.add_argument("--runs", type=int, default=30)
    parser.add_argument("--output", default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    results = {}
    with tempfile.TemporaryDirectory(prefix="xim-search-perf-", dir=work) as temporary:
        directory = Path(temporary)
        build_large_file(directory)
        build_project(directory / "project")
        runs = 1 if args.self_test else args.runs
        results["find_keystroke_to_highlight"] = summary(
            measure_find(binary, directory, "needle", 1000, runs))
        results["replace_all_1000_matches"] = summary(
            measure_replace(binary, directory, "needle", "NEEDLE", runs))
        project_samples, project_cold = measure_project(
            binary, directory / "project", "needle", runs)
        results["project_search_warm"] = summary(project_samples)
        results["project_search_cold"] = round(project_cold, 3)
        results["idle_one_second"] = measure_idle(binary, directory / "project")
    text = json.dumps(results, indent=2)
    print(text)
    if args.output:
        Path(args.output).write_text(text + "\n")


if __name__ == "__main__":
    main()
