#!/usr/bin/env python3
"""Completed native-shortcut latencies, compared with the minimal Vim engine."""
import argparse
import json
import statistics
import shutil
import tempfile
import time
from pathlib import Path
from test_xim import Session

CONFIG = r"""
set nocompatible noswapfile noshowmode noshowcmd noruler shortmess+=I ttimeout ttimeoutlen=20
set laststatus=2 selection=exclusive
set t_u7= t_RB= t_RF= t_RV= t_RK=
if !exists('syntax_on')
  syntax enable
endif
let g:phase = 'READY'
let g:expected = 'XBENCH_LINE_00001'
function! Observe()
  let done = (g:phase == 'READY' || g:phase == 'TYPE' || g:phase == 'PASTE')
        \ ? getline(1) ==# g:expected
        \ : g:phase == 'SELECT' ? getpos('v')[2] == 1 && col('.') == 2
        \ : g:phase == 'SCROLL' ? line('.') > 20 : 0
  if done
    if get(g:, 'delay', 0) > 0
      execute 'sleep ' . g:delay . 'm'
      let g:delay = 0
    endif
    if g:phase == 'SELECT'
      let g:selected = [string(getpos('v')[2]), string(col('.'))]
    endif
    let phase = g:phase
    let g:phase = ''
    redraw!
    echo 'XIM_' . 'DONE_' . phase
  endif
endfunction
autocmd TextChanged,TextChangedI,CursorMoved,CursorMovedI * call Observe()
"""


class ModalSession(Session):
    def ex(self, command):
        self.serial += 1
        marker = f"XIM_ACK_{self.serial}".encode()
        self.send("\x1b:" + command + f"|echo 'XIM_' . 'ACK_{self.serial}'\r")
        return self.wait(marker)


def sample(binary, root, native, startup_log=None, profile=None, delay=0, runtime=None):
    with tempfile.TemporaryDirectory(prefix="latency-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "bench.vim").write_text(CONFIG + f"\nlet g:delay = {delay}\n")
        fixture = [f"BENCH_LINE_{i:05d}" for i in range(1, 20001)]
        (directory / "fixture.txt").write_text("\n".join(fixture) + "\n")
        arguments = ["-u", "bench.vim", "-i", "NONE", "fixture.txt"]
        if startup_log:
            arguments += ["--startuptime", str(startup_log.resolve())]
        session = (Session if native else ModalSession)(binary, directory,
                    arguments, ready=b"BENCH_LINE_00001", runtime=None if native else runtime)
        result = {"first_paint": session.first_paint_ns / 1e6}

        def timed(phase, keys, marker=None):
            started = time.perf_counter_ns()
            session.send(keys)
            session.wait(marker or ("XIM_DONE_" + phase).encode())
            return (time.perf_counter_ns() - started) / 1e6

        try:
            elapsed = timed("READY", "X" if native else "iX")
            if delay:
                assert elapsed >= delay * .9, "acknowledgment arrived before delayed completion"
            result["first_accepted_edit"] = (time.perf_counter_ns() - session.started) / 1e6
            assert session.snapshot().splitlines()[0] == "X" + fixture[0]
            session.ex("call cursor(1,1)|let g:phase='TYPE'|let g:expected='YXBENCH_LINE_00001'")
            result["typing"] = timed("TYPE", "Y" if native else "iY")
            assert session.snapshot().splitlines()[0] == "YX" + fixture[0]
            session.ex("call cursor(1,1)|let g:phase='SELECT'")
            result["selection"] = timed("SELECT", b"\x1b[1;2C" if native else b"vl")
            session.ex("call writefile(g:selected, 'selection')")
            assert (directory / "selection").read_text().splitlines() == ["1", "2"]
            session.ex("call cursor(1,1)|let g:phase='SCROLL'")
            result["scroll"] = timed("SCROLL", b"\x1b[6~" if native else b"\x06")
            session.ex("call writefile([string(line('.'))], 'scroll')")
            assert int((directory / "scroll").read_text()) > 20
            session.ex("call cursor(1,1)|let g:phase='PASTE'|let g:expected=repeat('P',10240)..getline(1)")
            data = b"\x1b[200~" + b"P" * 10240 + b"\x1b[201~"
            result["paste_10k"] = timed("PASTE", data if native else b"i" + data)
            assert session.snapshot().splitlines()[0] == "P" * 10240 + "YX" + fixture[0]
            if native:
                result["command_ui"] = timed("PALETTE", b"\x10", b"Command:")
                session.send(b"Ex command\r")
                session.wait(b"Ex:")
                session.send("echo 'XIM_' . 'PALETTE_OK'\r")
                session.wait(b"XIM_PALETTE_OK")
            else:
                result["command_ui"] = timed("COMMAND", b"\x1b:", b":")
                session.send("echo 'XIM_' . 'COMMAND_OK'\r")
                session.wait(b"XIM_COMMAND_OK")
            if profile:
                session.send(b"\x10Ex command\r")
                session.wait(b"Ex:")
                session.send(b"qall!\r")
                session.process.wait(timeout=5)
        finally:
            session.close()
        if profile:
            shutil.copyfile(directory / "gmon.out", profile)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, default=Path(__file__).resolve().parent.parent / "runtime",
                        help="matching reference runtime; native uses its staged runtime")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=60)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--startup-log", type=Path)
    parser.add_argument("--profile-dir", type=Path)
    parser.add_argument("--self-test", action="store_true", help="delay first-edit completion by one second")
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    if args.runs < 1:
        parser.error("--runs must be positive")
    if args.profile_dir:
        args.profile_dir.mkdir(parents=True, exist_ok=True)
    values = {"native": [], "reference": []}
    for i in range(args.runs):
        order = ("native", "reference") if i % 2 == 0 else ("reference", "native")
        for label in order:
            binary = getattr(args, label).resolve()
            profile = args.profile_dir / f"native-{i}.gmon" if args.profile_dir and label == "native" else None
            startup_log = args.startup_log if i == 0 and label == "native" else None
            values[label].append(sample(binary, args.work.resolve(), label == "native", startup_log, profile,
                                        1000 if args.self_test else 0, args.runtime.resolve()))
    summary = {}
    for label, rows in values.items():
        summary[label] = {}
        for metric in rows[0]:
            numbers = sorted(row[metric] for row in rows)
            summary[label][metric] = {"median_ms": statistics.median(numbers),
                "p90_ms": numbers[min(len(numbers) - 1, int(len(numbers) * .9))]}
    output = {"runs": args.runs, "fixture_lines": 20000, "terminal": "xterm-256color 24x100",
               "native": str(args.native.resolve()), "reference": str(args.reference.resolve()),
               "reference_runtime": str(args.runtime.resolve()),
               "configuration": "-n -X -u bench.vim -i NONE; syntax enabled, laststatus=2, selection=exclusive",
               "profiled": bool(args.profile_dir), "delayed_self_test": args.self_test,
              "summary": summary, "samples": values,
              "command_ui_note": "Native palette versus Vim Ex prompt; different available interfaces."}
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
