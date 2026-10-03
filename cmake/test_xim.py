#!/usr/bin/env python3
"""PTY assertions for the native Xim executable, using real shortcuts."""
import argparse
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import time
from pathlib import Path


class Session:
    def __init__(self, binary, directory, arguments=(), ready=b"Xim", runtime=None):
        self.directory = directory
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        env = os.environ.copy()
        for name in ("VIM", "VIMRUNTIME", "VIMINIT", "EXINIT", "DISPLAY", "WAYLAND_DISPLAY"):
            env.pop(name, None)
        env.update(HOME=str(directory), XDG_CONFIG_HOME=str(directory / ".config"),
                   TERM="xterm-256color", LC_ALL="C.UTF-8")
        if runtime:
            env["VIMRUNTIME"] = str(runtime)
        # Deliberately enable terminal flow control before launch.
        attrs = termios.tcgetattr(slave)
        attrs[0] |= termios.IXON
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        self.started = time.perf_counter_ns()
        self.process = subprocess.Popen([str(binary), "-n", "-X", *arguments], cwd=directory,
                                        env=env, stdin=slave, stdout=slave, stderr=slave,
                                        start_new_session=True)
        os.close(slave)
        self.serial = 0
        try:
            self.wait(ready)
            self.first_paint_ns = time.perf_counter_ns() - self.started
            assert not termios.tcgetattr(self.master)[0] & termios.IXON, "Ctrl-S flow control still enabled"
        except BaseException:
            self.close()
            raise

    def send(self, text):
        if isinstance(text, str):
            text = text.encode()
        os.write(self.master, text)

    def wait(self, marker, timeout=5):
        output = bytearray()
        deadline = time.monotonic() + timeout
        while marker not in output:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"missing {marker!r}; output: {bytes(output[-3000:])!r}")
            ready, _, _ = select.select([self.master], [], [], min(remaining, .1))
            if ready:
                try:
                    output.extend(os.read(self.master, 65536))
                except OSError as error:
                    raise AssertionError(f"editor exited: {self.process.poll()}, {bytes(output)!r}") from error
        return bytes(output)

    def ex(self, command):
        self.serial += 1
        marker = f"XIM_ACK_{self.serial}".encode()
        self.send(b"\x10Ex command\r")
        self.wait(b"Ex:")
        self.send(command + f"|echo 'XIM_' . 'ACK_{self.serial}'\r")
        return self.wait(marker)

    def snapshot(self):
        self.ex("call writefile(getline(1, '$'), 'snapshot')")
        return (self.directory / "snapshot").read_text()

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        os.close(self.master)


def run(binary, root, themes=()):
    with tempfile.TemporaryDirectory(prefix="xim-pty-", dir=root) as temporary:
        directory = Path(temporary)
        # A Vim user/plugin stack must not be loaded by native startup.
        (directory / ".vimrc").write_text("call writefile(['bad'], 'user-vimrc-loaded')\n")
        session = Session(binary, directory)
        try:
            assert not (directory / "user-vimrc-loaded").exists()
            session.send("abc")
            assert session.snapshot() == "abc\n"
            session.send(b"\x1bZ")  # Escape never leaves native editing.
            assert session.snapshot() == "abcZ\n"
            session.send(b"\x01\x03")  # Select all, internal copy.
            session.send(b"\x1b\x16")
            assert session.snapshot() == "abcZabcZ\n"
            session.send(b"\x1a")
            assert session.snapshot() == "abcZ\n"
            session.send(b"\x19")
            assert session.snapshot() == "abcZabcZ\n"
            session.send(b"\x01new")
            assert session.snapshot() == "new\n"
            session.send(b"\x1a")
            assert session.snapshot() == "abcZabcZ\n", "replacement must be one undo operation"
            session.send(b"\x01\x18")
            assert session.snapshot() == "\n"
            session.send(b"\x16")
            assert session.snapshot() == "abcZabcZ\n"
            # Multi-line Unicode/tab paste, then a single undo/redo.
            session.send(b"\x01\x1b[200~" + "α\t界\nsecond\nthird".encode() + b"\x1b[201~")
            actual = session.snapshot()
            assert actual == "α\t界\nsecond\nthird\n", repr(actual)
            session.send(b"\x1a")
            assert session.snapshot() == "abcZabcZ\n"
            session.send(b"\x19")
            assert session.snapshot() == "α\t界\nsecond\nthird\n"
            # Shift selection across lines, then replacement.
            session.ex("call cursor(1,1)")
            session.send(b"\x1b[1;2B" + b"!")
            assert session.snapshot() == "!second\nthird\n"
            session.send(b"\x1a")
            assert session.snapshot() == "α\t界\nsecond\nthird\n"
            # Find and next/previous without modal commands.
            session.send(b"\x06third\r")
            session.ex("call writefile([string(line('.'))], 'position')")
            assert (directory / "position").read_text() == "3\n"
            # Save-as handles spaces and command metacharacters as filename text.
            name = "saved file|literal.txt"
            session.send(b"\x10Save as\r")
            session.wait(b"Save as:")
            session.send(name + "\r")
            session.ex("echo 'barrier'")
            assert (directory / name).read_text() == "α\t界\nsecond\nthird\n"
            session.send("Z\x13")
            session.ex("echo 'saved'")
            assert "Z" in (directory / name).read_text()
            # Unsaved quit can be cancelled, then ordinary typing still works.
            session.send("unsaved\x11")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            session.send("Q")
            assert "unsavedQ" in session.snapshot()
            # Cancel native prompts; neither text nor focus should be lost.
            before = session.snapshot()
            session.send(b"\x0fignored\x1b\x06ignored\x1b\x10ignored\x1b")
            assert session.snapshot() == before
            # Theme and Vimscript evaluation remain available.
            for theme in ("default", "desert", "slate", "habamax"):
                session.ex(f"colorscheme {theme}")
                session.ex("call writefile([g:colors_name, $VIMRUNTIME], 'theme')")
                lines = (directory / "theme").read_text().splitlines()
                assert lines[0] == theme
                assert Path(lines[1]).is_dir()
                session.ex("call writefile([string(synIDtrans(hlID('XimPrompt')) == hlID('Pmenu'))], 'link')")
                assert (directory / "link").read_text() == "1\n"
            for theme in themes:
                escaped = str(theme.resolve()).replace("'", "''")
                session.ex(f"set termguicolors|execute 'source ' . fnameescape('{escaped}')")
                session.ex("call writefile([g:colors_name, synIDattr(synIDtrans(hlID('XimPrompt')), 'bg', 'gui')], 'theme')")
                lines = (directory / "theme").read_text().splitlines()
                assert lines[0] == theme.stem and lines[1].startswith("#")
                print(f"External theme passed: {theme.stem}, prompt background {lines[1]}")
            session.ex("set nomodifiable")
            before = session.snapshot()
            session.send("blocked")
            assert session.snapshot() == before
            session.ex("set modifiable")
            # Open protects edits; cancellation retains the current buffer.
            (directory / "other.txt").write_text("other\n")
            before = session.snapshot()
            session.send(b"\x0fother.txt\r")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            assert session.snapshot() == before
            session.send(b"\x0fother.txt\r")
            session.wait(b"Save changes")
            session.send(b"d")
            assert session.snapshot() == "other\n"
            # UTF-8 prompt backspace removes the entire codepoint.
            session.send("\x06界\x7fother\r")
            session.ex("call writefile([string(col('.'))], 'position')")
            assert (directory / "position").read_text() == "1\n"
            # Unicode navigation/deletion and joins at line boundaries.
            session.send(b"\x01" + "α界\nsecond".encode())
            session.ex("call cursor(1,1)")
            session.send(b"\x1b[C\x1b[3~")
            assert session.snapshot() == "α\nsecond\n"
            session.ex("call cursor(2,1)")
            session.send(b"\x7f")
            assert session.snapshot() == "αsecond\n"
            session.send(b"\x1a")
            assert session.snapshot() == "α\nsecond\n"
            session.ex("call cursor(1,1)")
            session.send(b"\x1b[F\x1b[3~")
            assert session.snapshot() == "αsecond\n"
            # Down movement keeps the desired column across a short line.
            session.ex("call setline(1, ['0123456789', 'x', '0123456789'])|call cursor(1,8)")
            session.send(b"\x1b[B\x1b[B")
            session.ex("call writefile([string(col('.'))], 'position')")
            assert (directory / "position").read_text() == "8\n"
            session.ex("call setline(1, repeat('w', 180))|call cursor(1,181)")
            session.send(b"\x1b[D\x7f")
            assert len(session.snapshot().splitlines()[0]) == 179
            session.ex("set autoindent")
            session.send(b"\x01\x1b[200~  first\nsecond\x1b[201~")
            actual = session.snapshot()
            assert actual == "  first\nsecond\n", repr(actual)
            session.ex("call writefile([string(&autoindent)], 'setting')")
            assert (directory / "setting").read_text() == "1\n"
            session.ex("set noautoindent")
            before = session.snapshot()
            session.send(b"\x01" + b"t" * 600)
            session.send(b"\x1a")
            actual = session.snapshot()
            assert actual == "t" * 512 + "\n", (len(actual), repr(actual[-200:]))
            session.send(b"\x1a")
            assert session.snapshot() == "t" * 256 + "\n"
            session.send(b"\x1a")
            assert session.snapshot() == before
            session.ex("help xim-start|call writefile([expand('%:t')], 'help')")
            assert (directory / "help").read_text() == "xim.txt\n"
            print("Native PTY workflow passed")
        finally:
            session.close()
        readonly = directory / "readonly.txt"
        readonly.write_text("original\n")
        session = Session(binary, directory, ("-R", str(readonly)))
        try:
            session.send("changed\x13")
            session.wait(b"E45")
            assert readonly.read_text() == "original\n"
            assert session.snapshot() == "changedoriginal\n"
            session.send(b"\x11")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"E45")
            session.send(b"\x1b")
            assert session.snapshot() == "changedoriginal\n"
            session.send(b"\x10Save as\r")
            session.wait(b"Save as:")
            session.send("absent/parent/file.txt\r")
            session.wait(b"E212")
            session.send(b"\x1b")
            time.sleep(.05)
            assert session.snapshot() == "changedoriginal\n"
        finally:
            session.close()
        config = directory / "explicit.vim"
        config.write_text("let g:xim_config_loaded = 42\ninoremap Z wrong\n")
        session = Session(binary, directory, ("-u", str(config)))
        try:
            session.send("Z")
            assert session.snapshot() == "Z\n", "modal mappings are not native input policy"
            session.ex("call writefile([string(g:xim_config_loaded)], 'config')")
            assert (directory / "config").read_text() == "42\n"
        finally:
            session.close()
        subprocess.run([str(binary), "--vim", "-X", "-u", str(config), "-i", "NONE", "-es",
                        "-c", "call writefile([string(g:xim_config_loaded)], 'compat')",
                        "-c", "qall!"], cwd=directory, check=True, timeout=10)
        assert (directory / "compat").read_text() == "42\n"
        close_file = directory / "close.txt"
        close_file.write_text("close me\n")
        session = Session(binary, directory, (str(close_file),))
        try:
            session.send("changed\x17")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            assert session.snapshot() == "changedclose me\n"
            session.send(b"\x13")
            session.ex("echo 'saved'")
            assert close_file.read_text() == "changedclose me\n"
            session.send(b"\x17")
            assert session.snapshot() == "\n"
            session.send("next\x11")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"Save as:")
            session.send("from-confirm.txt\r")
            assert session.process.wait(timeout=5) == 0
            assert (directory / "from-confirm.txt").read_text() == "next\n"
        finally:
            session.close()
        # Successful writes never move the insertion position.
        position = directory / "cursor.txt"
        position.write_text("αβ\n")
        session = Session(binary, directory, (str(position),))
        try:
            session.send(b"\x01draft")
            assert session.snapshot() == "draft\n"
            session.send(b"\x13")
            session.ex("echo 'written'")
            assert position.read_text() == "draft\n"
            session.send("X")
            assert session.snapshot() == "draftX\n"
            session.send(b"\x13")
            session.ex("echo 'written'")
            assert position.read_text() == "draftX\n"
        finally:
            session.close()
        session = Session(binary, directory)
        try:
            session.send("draft")
            session.send(b"\x13")
            session.wait(b"Save as:")
            session.send("saved-as.txt\r")
            session.ex("echo 'written'")
            assert (directory / "saved-as.txt").read_text() == "draft\n"
            session.send("X")
            assert session.snapshot() == "draftX\n"
        finally:
            session.close()
        session = Session(binary, directory)
        try:
            session.send(b"\x1b[200~" + "α😀\r\nβ".encode() + b"\x1b[201~")
            session.send(b"\x13")
            session.wait(b"Save as:")
            session.send("unicode-lines.txt\r")
            session.ex("echo 'written'")
            session.send("X")
            assert session.snapshot() == "α😀\nβX\n"
        finally:
            session.close()
        # A failed unnamed quit confirmation keeps its choices.
        session = Session(binary, directory)
        try:
            session.send("keep")
            session.send(b"\x11")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"Save as:")
            session.send("missing/parent/quit.txt\r")
            session.wait(b"E212")
            session.wait(b"Save changes")
            session.send(b"d")
            assert session.process.wait(timeout=5) == 0
            assert not Path(directory / "missing/parent/quit.txt").exists()
        finally:
            session.close()
        # Empty Save-as names cannot hide the pending operation.
        session = Session(binary, directory)
        try:
            session.send("keep")
            session.send(b"\x11")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"Save as:")
            session.send(b"\r")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            assert "keep" in session.snapshot().split()
            session.send(b"\x13")
            session.wait(b"Save as:")
            session.send("empty-saved.txt\r")
            session.ex("echo 'not-quitting'")
            assert session.process.poll() is None
            assert (directory / "empty-saved.txt").read_text() == "keep\n"
        finally:
            session.close()
        # An unnamed open continuation keeps fallback choices after a failed save.
        other = directory / "other.txt"
        other.write_text("replace me\n")
        session = Session(binary, directory)
        try:
            session.send("changed")
            session.send(b"\x0fother.txt\r")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"Save as:")
            session.send("missing/parent/opening.txt\r")
            session.wait(b"E212")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            assert session.snapshot() == "changed\n"
            session.send(b"\x13")
            session.wait(b"Save as:")
            session.send("saved-before-open.txt\r")
            session.ex("echo 'saved'")
            assert (directory / "saved-before-open.txt").read_text() == "changed\n"
            session.send(b"\x0fother.txt\r")
            session.ex("echo 'opened'")
            assert session.snapshot() == "replace me\n"
        finally:
            session.close()
        # An unnamed close continuation leaves a visible recovery path.
        session = Session(binary, directory)
        try:
            session.send("closeme")
            session.send(b"\x17")
            session.wait(b"Save changes")
            session.send(b"s")
            session.wait(b"Save as:")
            session.send("missing/parent/closing.txt\r")
            session.wait(b"E212")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            time.sleep(.05)
            assert session.snapshot() == "closeme\n"
            session.send(b"\x10Save as\r")
            session.wait(b"Save as:")
            session.send("closed-ok.txt\r")
            session.ex("echo 'saved'")
            assert session.snapshot() == "closeme\n"
            assert (directory / "closed-ok.txt").read_text() == "closeme\n"
            session.send(b"\x17")
            assert session.snapshot() == "\n"
        finally:
            session.close()
        print("Read-only, failed save, configuration and compatibility checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--theme", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    run(args.binary.resolve(), args.work.resolve(), args.theme)
