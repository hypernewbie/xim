#!/usr/bin/env python3
"""Completed Plan 6 live-project latencies.

Serial runs only. Explorer and file-operation samples use one warm process;
the 100000-child first-page samples start a fresh process per sample so the
directory cache is empty. Each timer ends on a visible completion, not on an
echoed command.
"""
import argparse
import json
import os
import select
import statistics
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
        "samples": len(samples),
    }


def marker_line(screen):
    for line in screen.split(b"\n"):
        stripped = line.strip()
        if stripped.startswith(b">"):
            return stripped
    return None


def build_project(directory, files=10000, directories=10):
    directory.mkdir(parents=True, exist_ok=True)
    for index in range(directories):
        (directory / f"dir_{index:02}").mkdir(exist_ok=True)
    for index in range(files):
        (directory / f"file_{index:05}.txt").touch()


def build_wide_directory(directory, files=100000):
    directory.mkdir(parents=True, exist_ok=True)
    for index in range(files):
        descriptor = os.open(directory / f"file_{index:06}.txt",
                             os.O_CREAT | os.O_WRONLY | os.O_CLOEXEC, 0o644)
        os.close(descriptor)


def open_explorer(session, expected):
    session.send(b"\x05")  # Ctrl-E
    assert wait_fast(session, lambda: b"Explorer:" in session.screen.text()
              and expected in session.screen.text()
              and marker_line(session.screen.text()) is not None), session.screen.text()
    assert wait_fast(session,
              lambda: b"Refreshing explorer" not in session.screen.text()), session.screen.text()


def measure_movement(session, runs):
    samples = []
    previous = marker_line(session.screen.text())
    assert previous is not None, "explorer selection marker missing"
    for _ in range(runs):
        started = time.perf_counter_ns()
        session.send(b"\x1b[B")  # Down
        assert wait_fast(session, lambda: marker_line(session.screen.text())
                not in (None, previous)), session.screen.text()
        samples.append((time.perf_counter_ns() - started) / 1e6)
        current = marker_line(session.screen.text())
        assert current is not None and current != previous, \
            "explorer selection did not move to a new row"
        previous = current
    return samples


def measure_first_page(binary, wide, runs):
    samples = []
    for _ in range(runs):
        session = Session(binary, wide, (".",))
        try:
            # A current quick-open result proves index publication. This
            # does not populate the independent explorer directory cache.
            session.send(b"\x10file_000000.txt")
            assert wait_fast(session,
                    lambda: b"Files: file_000000.txt" in session.screen.text()
                    and b"> file_000000.txt" in session.screen.text(),
                    timeout=30), session.screen.text()
            session.send(b"\x1b")
            assert wait_fast(session,
                    lambda: b"Files:" not in session.screen.text()), session.screen.text()
            assert b"file_000000.txt" not in session.screen.text()
            started = time.perf_counter_ns()
            session.send(b"\x05")
            assert wait_fast(session, lambda: b"Explorer:" in session.screen.text()
                      and b"file_000000.txt" in session.screen.text(),
                      timeout=10), session.screen.text()
            samples.append((time.perf_counter_ns() - started) / 1e6)
        finally:
            session.close()
    return samples


def measure_external_change(session, project, runs, token):
    samples = []
    for index in range(runs):
        name = f"000_external_{token}_{index:02}.txt"
        started = time.perf_counter_ns()
        (project / name).write_text("external\n")
        assert wait_fast(session, lambda: name.encode() in session.screen.text()
                  and b"Refreshing explorer" not in session.screen.text()), session.screen.text()
        assert name.encode() in session.screen.text(), \
            "external create did not reach the visible explorer"
        samples.append((time.perf_counter_ns() - started) / 1e6)
        # Remove the entry before the next sample so the first page and the
        # selection stay at the top; the removal itself is not timed.
        (project / name).unlink()
        assert wait_fast(session,
                lambda: name.encode() not in session.screen.text()), session.screen.text()
    return samples


def create_once(session, project, name, timed):
    assert not (project / name).exists(), "file-operation name is not fresh"
    session.send(b"\x1bOQ")  # F2 -> command palette
    session.wait(b"Command: ")
    session.send(b"New file\r")
    session.wait(b"New file:")
    started = time.perf_counter_ns()
    session.send(name.encode() + b"\r")
    assert wait_fast(session, lambda: (b"Xim " + name.encode()) in session.screen.text()
              or (b"Created " + name.encode()) in session.screen.text()), session.screen.text()
    assert (project / name).exists(), "created file missing on disk"
    assert (b"Xim " + name.encode()) in session.screen.text() \
        or (b"Created " + name.encode()) in session.screen.text(), \
        "file creation produced no visible completion"
    return (time.perf_counter_ns() - started) / 1e6 if timed else 0.0


def measure_create(session, project, runs, token):
    samples = []
    # The picker owns keys while it is open; return to the document first.
    session.send(b"\x1b")
    assert wait_fast(session,
            lambda: b"Explorer:" not in session.screen.text()), session.screen.text()
    # One untimed operation absorbs first-use allocation and filesystem
    # writeback from fixture creation; the plan requires no overlapping work.
    create_once(session, project, f"op_{token}_warm.txt", False)
    for index in range(runs):
        samples.append(create_once(session, project,
                                   f"op_{token}_{index:02}.txt", True))
    return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--project-files", type=int, default=10000)
    parser.add_argument("--wide-files", type=int, default=100000)
    parser.add_argument("--movement-runs", type=int, default=60)
    parser.add_argument("--first-page-runs", type=int, default=10)
    parser.add_argument("--external-runs", type=int, default=30)
    parser.add_argument("--create-runs", type=int, default=30)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    project = args.work.resolve() / "live-project"
    wide = args.work.resolve() / "wide-directory"
    if not (project / "file_00000.txt").exists():
        build_project(project, args.project_files)
    if not (wide / "file_000000.txt").exists():
        build_wide_directory(wide, args.wide_files)
    os.sync()

    movement = []
    external = []
    create = []
    token = str(time.time_ns() % 100000)
    session = Session(args.binary.resolve(), project, (".",))
    try:
        open_explorer(session, b"dir_00")
        external = measure_external_change(session, project, args.external_runs,
                                           token)
        movement = measure_movement(session, args.movement_runs)
        create = measure_create(session, project, args.create_runs, token)
        # Validate the last created buffer is the visible document.
        assert b"op_" in session.screen.text(), "created file is not visible"
        session.send(b"\x11")
        session.process.wait(timeout=5)
    finally:
        session.close()
    first_page = measure_first_page(args.binary.resolve(), wide,
                                    args.first_page_runs)
    result = {
        "binary": str(args.binary.resolve()),
        "project_fixture": f"{args.project_files} files and 10 directories in the root",
        "wide_fixture": f"{args.wide_files} files in one directory",
        "terminal": "xterm-256color 24x100",
        "completion": "visible selection marker, visible explorer row, or created filename",
        "explorer_cached_movement_ms": summary(movement),
        "explorer_first_page_ms": summary(first_page),
        "external_change_visible_ms": summary(external),
        "file_create_visible_ms": summary(create),
    }
    payload = dict(result)
    payload["samples_ms"] = {
        "explorer_cached_movement": movement,
        "explorer_first_page": first_page,
        "external_change_visible": external,
        "file_create_visible": create,
    }
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
