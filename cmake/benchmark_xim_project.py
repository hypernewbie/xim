#!/usr/bin/env python3
"""Completed quick-open latency on a warmed 100000-path project fixture."""
import argparse
import json
import os
import select
import statistics
import tempfile
import time
from pathlib import Path
from test_xim import Session


def wait_screen(session, predicate, timeout=10):
    until = time.monotonic() + timeout
    while not predicate(session.screen.text()):
        remaining = until - time.monotonic()
        if remaining <= 0:
            raise AssertionError(f"screen completion timed out: {session.screen.text()[-2000:]!r}")
        ready, _, _ = select.select([session.master], [], [], remaining)
        if ready:
            session.screen.feed(os.read(session.master, 65536))


def cpu_ticks(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().split()
    return int(fields[13]) + int(fields[14])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=60)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    samples = []
    editing = []
    with tempfile.TemporaryDirectory(prefix="project-latency-", dir=args.work.resolve()) as temporary:
        project = Path(temporary)
        # Exactly 100000 indexed files: two exact basenames plus two
        # 49999-file families distributed over 200 directories.
        (project / "alpha.txt").write_text("ALPHA\n")
        (project / "beta.txt").write_text("BETA\n")
        for family in ("alpha", "beta"):
            for directory in range(100):
                (project / f"{family}_{directory:03d}").mkdir()
            for i in range(49999):
                (project / f"{family}_{i % 100:03d}" / f"{family}_{i:05d}.txt").touch()
        session = Session(args.binary.resolve(), project, arguments=(".",))
        try:
            session.send(b"\x10alpha")
            wait_screen(session, lambda text: b"> alpha.txt" in text)
            # Index publication is whole-snapshot, so an indexed match
            # establishes warm index completion, not just first paint.
            for i in range(args.runs):
                family = "beta" if i % 2 == 0 else "alpha"
                started = time.perf_counter_ns()
                session.send(b"\x1b\x10" + family.encode())
                wait_screen(session, lambda text: ("Files: " + family).encode() in text
                    and ("> " + family + ".txt").encode() in text)
                samples.append((time.perf_counter_ns() - started) / 1e6)
                assert b"Indexing project..." not in session.screen.text()
            before = cpu_ticks(session.process.pid)
            idle_output = session.drain(1)
            idle_ticks = cpu_ticks(session.process.pid) - before
            assert not idle_output, "idle picker generated terminal output"
            status = Path(f"/proc/{session.process.pid}/status").read_text()
            rss = next(line for line in status.splitlines() if line.startswith("VmRSS:"))
            session.send(b"\r")
            expected = "ALPHA\n" if args.runs % 2 == 0 else "BETA\n"
            assert session.snapshot() == expected, "selected visible result did not open"
            session.send(b"\x11")
            session.process.wait(timeout=2)
        finally:
            session.close()
        config = args.work.resolve() / "active-scan.vim"
        config.write_text("""set shortmess+=I t_u7= t_RB= t_RF= t_RV= t_RK=
function! Edited()
  if getline(1) ==# 'X'
    redraw!
    echo 'XIM_' . 'EDIT_READY'
  endif
endfunction
autocmd TextChanged * call Edited()
""")
        for _ in range(args.runs):
            session = Session(args.binary.resolve(), project, arguments=("-u", str(config), "."))
            try:
                session.send(b"X")
                session.wait(b"XIM_EDIT_READY")
                editing.append((time.perf_counter_ns() - session.started) / 1e6)
            finally:
                session.close()  # cancels the still-active large scan
                assert session.process.returncode != -9, "shutdown required a forced kill"
    ordered = sorted(samples)
    result = {"binary": str(args.binary.resolve()), "runs": args.runs,
        "indexed_paths": 100000, "shape": "200 directories, two 49999-file families plus two root files",
        "cache": "new fixture on local filesystem, warmed published path index; no disk-cache flush",
        "terminal": "xterm-256color 24x100", "completion": "current query label and exact selected result visible; activation content verified",
        "median_ms": statistics.median(samples), "p90_ms": ordered[int(len(ordered) * .9)],
        "project_first_accepted_edit_median_ms": statistics.median(editing),
        "project_first_accepted_edit_p90_ms": sorted(editing)[int(len(editing) * .9)],
        "samples_ms": samples, "project_first_accepted_edit_samples_ms": editing,
        "idle_cpu_ticks_1s": idle_ticks,
        "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"), "idle_output_bytes_1s": len(idle_output), "rss": rss}
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if not key.endswith("samples_ms")}, indent=2))


if __name__ == "__main__":
    main()
