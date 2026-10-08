#!/usr/bin/env python3
"""PTY assertions for the native Xim executable, using real shortcuts."""
import argparse
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import time
from pathlib import Path
from terminal_screen import TerminalScreen


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
        self.screen = TerminalScreen()
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
        # Incremental overlays may render a marker once and emit no further
        # bytes, so check the retained screen before every read.
        output = bytearray()
        deadline = time.monotonic() + timeout
        while True:
            if marker in output or marker in self.screen.text():
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"missing {marker!r}; output: {bytes(output[-3000:])!r}")
            ready, _, _ = select.select([self.master], [], [], min(remaining, .1))
            if ready:
                try:
                    data = os.read(self.master, 65536)
                    self.screen.feed(data)
                    output.extend(data)
                except OSError as error:
                    raise AssertionError(f"editor exited: {self.process.poll()}, {bytes(output)!r}") from error
        return bytes(output) + b"\n" + self.screen.text()

    def wait_for_file(self, path, contents, timeout=5):
        """Wait for a file the editor writes, draining the terminal meanwhile."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.drain(.1)
            if path.exists() and path.read_text().strip() == contents:
                return True
        return path.exists() and path.read_text().strip() == contents

    def drain(self, timeout=.5):
        """Read whatever arrives within the timeout without requiring a marker."""
        output = bytearray()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.master], [], [], min(.1, max(0, deadline - time.monotonic())))
            if ready:
                try:
                    data = os.read(self.master, 65536)
                    self.screen.feed(data)
                    output.extend(data)
                except OSError:
                    break
        return bytes(output)

    def ex(self, command):
        self.serial += 1
        # Confirm completion through a file: a redraw can clear the message
        # line before an echo marker is read, which made this wait slow.
        ack = Path(tempfile.gettempdir()) / f"xim_ack_{os.getpid()}_{self.serial}"
        try:
            ack.unlink()
        except FileNotFoundError:
            pass
        self.send(b"\x1bOPEx command\r")
        self.wait(b"Ex:")
        self.send(command + f"|call writefile(['{self.serial}'], '{ack}')\r")
        if not self.wait_for_file(ack, str(self.serial), timeout=15):
            raise AssertionError(f"Ex command did not finish: {command!r}")
        return b""

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
            session.send(b"\x1bOPSave as\r")
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
            session.send(b"\x0fignored\x1b\x06ignored\x1b\x1bOPignored\x1b")
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
            session.send(b"\x1bOPSave as\r")
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
            session.send(b"\x1bOPSave as\r")
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


def project_run(binary, root):
    """Plan 2: project root, quick open, explorer and buffer picker."""
    with tempfile.TemporaryDirectory(prefix="xim-project-", dir=root) as temporary:
        project = Path(temporary)
        (project / "src" / "deep").mkdir(parents=True)
        (project / "src" / "alpha.cpp").write_text("alpha\n")
        (project / "src" / "deep" / "beta.cpp").write_text("beta\n")
        (project / "top.md").write_text("top\n")
        # Ignored trees must never appear in quick open.
        (project / ".git").mkdir()
        (project / ".git" / "hidden.cpp").write_text("hidden\n")
        (project / "node_modules").mkdir()
        (project / "node_modules" / "dep.cpp").write_text("dep\n")

        # A directory argument roots the project instead of editing ".".
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"Files:")
            session.send(b"alpha")
            session.wait(b"alpha.cpp")
            session.send(b"\r")
            session.ex("echo 'opened'")
            assert session.snapshot() == "alpha\n"
            assert session.snapshot() == "alpha\n", "quick open must not edit text"
            # Explicit path activation is checked against the real file.
            assert (project / "src" / "alpha.cpp").read_text() == "alpha\n"
            # Ignored trees stay out of the index.
            session.send(b"\x10")
            session.wait(b"Files:")
            session.send(b"hidden")
            time.sleep(.3)
            assert b"hidden.cpp" not in session.drain(), "ignored tree leaked"
            session.send(b"\x1b")
        finally:
            session.close()

        # Explorer expands a directory and opens a nested file.
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x05")            # Ctrl-E -> explorer
            screen = session.wait(b"src/")
            assert b"Explorer:" in screen, "explorer prompt missing"
            session.send(b"\r")              # expand the selected directory
            session.wait(b"alpha.cpp")
            session.send(b"alpha\r")
            session.ex("echo 'explored'")
            assert session.snapshot() == "alpha\n"
            # Buffer picker lists the files opened so far.
            session.send(b"\x10beta\r")
            session.ex("echo 'beta'")
            assert session.snapshot() == "beta\n"
            session.send(b"\x1bOPBuffers\r")
            screen = session.wait(b"Buffers:")
            assert b"alpha.cpp" in screen, "buffer picker lost an open file"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Project root, quick open, explorer and buffer checks passed")


def project_gitignore(binary, root):
    """Plan 2 closeout: .gitignore patterns, negation and inner overrides."""
    with tempfile.TemporaryDirectory(prefix="xim-gitignore-", dir=root) as temporary:
        project = Path(temporary)
        (project / "src").mkdir()
        (project / ".gitignore").write_text("*.log\n")
        (project / "src" / "keep.txt").write_text("keep\n")
        (project / "src" / "run.log").write_text("run\n")
        (project / "src" / ".gitignore").write_text("!keep.log\n")
        (project / "src" / "keep.log").write_text("never\n")

        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            screen = session.wait(b"keep.txt")
            assert b"keep.log" in screen, "inner !keep.log must override root *.log"
            assert b"run.log" not in screen, "root *.log must hide run.log"
            session.send(b"\x1b")
        finally:
            session.close()
        print(".gitignore, negation and inner override checks passed")


def project_symlinks(binary, root):
    """Plan 2 closeout: symlink cycles and root escapes never recurse."""
    with tempfile.TemporaryDirectory(prefix="xim-symlinks-", dir=root) as temporary:
        project = Path(temporary)
        (project / "real").mkdir()
        (project / "real" / "alpha.cpp").write_text("alpha\n")
        # Self-link must not cause infinite recursion.
        os.symlink(".", project / "loop", target_is_directory=True)
        # A symlink to a directory outside the root must not escape.
        outside = Path(tempfile.mkdtemp(prefix="xim-symlinks-outside-", dir=root))
        (outside / "should_not_appear.cpp").write_text("leak\n")
        os.symlink(outside, project / "escape", target_is_directory=True)
        # A regular file symlink is still indexed.
        os.symlink("real/alpha.cpp", project / "alias.cpp")

        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            screen = session.wait(b"alpha.cpp")
            assert b"alias.cpp" in screen, "file symlink not indexed under its own name"
            assert b"real/alpha.cpp" in screen, "real file missing from the index"
            assert b"should_not_appear" not in screen, "escape leaked outside root"
            assert b"loop" not in screen and b"escape" not in screen
            session.send(b"\x1b")
            session.send(b"\x05")            # Ctrl-E -> explorer
            screen = session.wait(b"Explorer:")
            assert b"loop" not in screen, "self-link listed in explorer"
            assert b"escape" not in screen, "escape listed in explorer"
            assert b".." not in screen, "explorer listed a path outside the root"
            # A file symlink opens its target under the in-tree name.
            session.send(b"alias.cpp\r")
            session.wait(b"alpha")
            assert session.snapshot() == "alpha\n", "symlink did not open its target"
        finally:
            session.close()
        shutil.rmtree(outside)
        print("Symlink cycle and root-escape checks passed")


def project_explicit_path(binary, root):
    """Plan 2 closeout: an absolute path opens the file directly."""
    with tempfile.TemporaryDirectory(prefix="xim-explicit-", dir=root) as temporary:
        project = Path(temporary)
        (project / "inside.cpp").write_text("alpha\n")
        absolute = (project / "inside.cpp").resolve()
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"Files:")
            # Type the whole absolute path.  Every intermediate prefix
            # must stay on a valid result range: the first character is
            # a bare "/", which used to sort past an empty candidate
            # list and crash the editor.
            session.send(str(absolute).encode() + b"\r")
            session.wait(b"alpha")
            assert session.snapshot() == "alpha\n", "path did not open the file"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Explicit-path opening check passed")


def project_single_file_no_scan(binary, root):
    """Plan 2 closeout: a file argument does not start a directory walk."""
    with tempfile.TemporaryDirectory(prefix="xim-single-", dir=root) as temporary:
        project = Path(temporary)
        (project / "hello.txt").write_text("hello\n")
        (project / "sibling.txt").write_text("sibling\n")
        target = project / "hello.txt"
        session = Session(binary, project, arguments=(str(target),))
        try:
            assert session.snapshot() == "hello\n", "file argument did not open"
            # A name query must find nothing: no walk ever ran.
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"Files:")
            session.send(b"sibling")
            output = session.drain(0.4)
            assert b"> " not in output, "file-only start indexed a sibling"
            assert b"Indexing" not in output, "file-only start claimed indexing"
            # An absolute path still opens through the picker.
            session.send(b"\x1b")
            session.send(b"\x10")
            session.wait(b"Files:")
            session.send(str(target.resolve()).encode() + b"\r")
            session.wait(b"hello")
            assert session.snapshot() == "hello\n", "absolute path did not reopen"
        finally:
            session.close()
        print("Single-file invocation skips the walk; absolute paths open")


def project_refresh_preserves_expansion(binary, root):
    """Plan 2 closeout: refresh keeps the explorer's expansion state."""
    with tempfile.TemporaryDirectory(prefix="xim-refresh-", dir=root) as temporary:
        project = Path(temporary)
        (project / "src").mkdir()
        (project / "src" / "alpha.cpp").write_text("alpha\n")
        (project / "beta.cpp").write_text("beta\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x05")            # Ctrl-E -> explorer
            session.wait(b"src/")
            session.send(b"\r")              # expand src/
            session.wait(b"alpha.cpp")
            session.send(b"\x1b")
            # A real native refresh must discover a file created after
            # the initial scan, not merely reopen an unchanged buffer.
            (project / "src" / "new.cpp").write_text("new\n")
            session.send(b"\x1bOPRefresh project\r")
            session.send(b"\x10new")
            session.wait(b"src/new.cpp")
            session.send(b"\x1b")
            session.send(b"\x1bOPExplorer\r")
            screen = session.wait(b"Explorer:")
            assert b"alpha.cpp" in screen, "expansion lost after refresh"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Refresh preserves expansion check passed")


def project_gitignore_anchored(binary, root):
    """Plan 2 closeout: an anchored rule matches one component only."""
    with tempfile.TemporaryDirectory(prefix="xim-anchored-", dir=root) as temporary:
        project = Path(temporary)
        (project / "cache").mkdir(parents=True)
        (project / "src" / "cache").mkdir(parents=True)
        (project / ".gitignore").write_text("/cache/\n")
        (project / "cache" / "root.txt").write_text("ignored\n")
        (project / "src" / "cache" / "kept.txt").write_text("kept\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"Files:")
            session.send(b"cache")           # force a re-render after the scan
            output = session.drain(0.4)
            assert b"src/cache/kept.txt" in output, "anchored rule hit a deeper directory"
            assert b"cache/root.txt" not in output, "anchored rule missed its own directory"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Anchored gitignore scoping check passed")


def project_gitignore_scoped(binary, root):
    """Plan 2 closeout: an inner .gitignore applies to its own directory."""
    with tempfile.TemporaryDirectory(prefix="xim-scoped-", dir=root) as temporary:
        project = Path(temporary)
        (project / "a").mkdir()
        (project / "a" / ".gitignore").write_text("a\n")
        (project / "a" / "keep.txt").write_text("kept\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"Files:")
            session.send(b"keep")            # force a re-render after the scan
            output = session.drain(0.4)
            assert b"a/keep.txt" in output, "inner rule wrongly ignored a child"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Inner .gitignore scoping check passed")


def project_picker_arrows(binary, root):
    """Plan 2 closeout: arrow keys select in the file picker."""
    with tempfile.TemporaryDirectory(prefix="xim-arrows-", dir=root) as temporary:
        project = Path(temporary)
        (project / "a.txt").write_text("A\n")
        (project / "b.txt").write_text("B\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            session.wait(b"b.txt")
            session.send(b"\x1b[B")          # Down
            output = session.drain(0.3)
            assert b"> b.txt" in output, "Down did not move the selection"
            session.send(b"\r")
            session.wait(b"2B")
            assert session.snapshot() == "B\n", "Enter opened the first entry, not the selected one"
        finally:
            session.close()
        print("Picker arrow selection check passed")


def project_dirty_switch(binary, root):
    """Plan 2 closeout: picking a file keeps an unsaved buffer in the list."""
    with tempfile.TemporaryDirectory(prefix="xim-dirty-", dir=root) as temporary:
        project = Path(temporary)
        (project / "a.txt").write_text("A\n")
        (project / "b.txt").write_text("B\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10a\r")         # open a.txt
            session.wait(b"2B")
            session.send(b"dirty")           # modify it
            session.send(b"\x10b\r")         # pick b.txt: no prompt
            output = session.wait(b"2B")
            assert b"Save changes" not in output, "switch prompted for the dirty buffer"
            assert session.snapshot() == "B\n", "switch did not open the picked file"
            session.send(b"\x1bOQ")          # F2 -> palette
            session.wait(b"Command:")
            session.send(b"Buffers\r")       # documented Ctrl-Shift-B fallback
            output = session.wait(b"Buffers:")
            output += session.drain(0.4)
            assert b"a.txt" in output and b"b.txt" in output, "dirty buffer vanished from the list"
            session.send(b"\x1b")
            session.send(b"\x11")            # Ctrl-Q: unsaved work still protects
            session.wait(b"Save changes")
        finally:
            session.close()
        print("Dirty-switch retention and quit protection checks passed")


def project_duplicate_basenames(binary, root):
    """Plan 2 closeout: two files with the same basename are distinguishable."""
    with tempfile.TemporaryDirectory(prefix="xim-duplicate-", dir=root) as temporary:
        project = Path(temporary)
        (project / "a").mkdir()
        (project / "b").mkdir()
        (project / "a" / "foo.cpp").write_text("x\n")
        (project / "b" / "foo.cpp").write_text("y\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10")            # Ctrl-P -> quick open
            screen = session.wait(b"foo.cpp")
            assert b"a/foo.cpp" in screen, "first duplicate missing"
            assert b"b/foo.cpp" in screen, "second duplicate missing"
            session.send(b"\x1b")
        finally:
            session.close()
        print("Duplicate-basename disambiguation check passed")


def project_background(binary, root):
    """Idle completion, query revisions, queued input and worker shutdown."""
    with tempfile.TemporaryDirectory(prefix="xim-background-", dir=root) as temporary:
        project = Path(temporary)
        (project / "files").mkdir()
        for i in range(7500):
            (project / "files" / f"alpha_{i:05d}.txt").touch()
        (project / "idle_target.txt").write_text("TARGET\n")
        session = Session(binary, project, arguments=(".",))
        try:
            session.send(b"\x10idle_target")
            # No follow-up key: both scan and match completion must wake.
            session.wait(b"idle_target.txt")
            session.drain(.1)
            assert session.drain(.25) == b"", "idle picker repeatedly redraws"
            # Send an obsolete query, then replace it and immediately Enter.
            # Subsequent typing belongs to the opened file, not the picker.
            session.send(b"\x1b\x10obsolete\x1b\x10idle_target\rPOST")
            assert session.snapshot() == "POSTTARGET\n", "stale query or input after Enter lost"
            # Bracketed paste following Enter keeps its payload and undo
            # boundary even if the current query has not completed yet.
            session.send(b"\x10idle_target\r\x1b[200~" + "界\n".encode() + b"\x1b[201~")
            assert "界\n" in session.snapshot(), "paste after Enter was lost"
            session.send(b"\x11")
            session.wait(b"Save changes")
            session.send(b"d")
            session.process.wait(timeout=2)
            assert session.process.returncode == 0, "worker shutdown failed"
        finally:
            session.close()
        print("Idle background completion, revisions and queued-input checks passed")


def buffer_picker_identity(binary, root):
    """Unnamed buffers have stable IDs; displayed names are not identities."""
    with tempfile.TemporaryDirectory(prefix="xim-buffer-identity-", dir=root) as temporary:
        project = Path(temporary)
        session = Session(binary, project)
        try:
            session.send(b"first")
            session.ex("hide enew")
            session.send(b"second")
            assert session.snapshot() == "second\n"
            session.send(b"\x1bOPBuffers\r")
            session.wait(b"Buffers:")
            visible = session.screen.text()
            assert b"1: + [No Name]" in visible, "first unnamed modified buffer missing"
            assert b"2:*+ [No Name]" in visible, "second unnamed active buffer missing"
            session.send(b"\r")
            assert session.snapshot() == "first\n", "unnamed picker switched by ambiguous name"
            session.send(b"\x1bOPBuffers\r")
            session.wait(b"Buffers:")
            session.send(b"\x1b[B\r")
            assert session.snapshot() == "second\n", "second unnamed identity changed"
        finally:
            session.close()
        print("Stable unnamed buffer IDs and modified/active markers checked")


def ui_incremental(binary, root):
    """Do not erase the editor/unchanged menu while typing in a prompt."""
    with tempfile.TemporaryDirectory(prefix="xim-ui-", dir=root) as temporary:
        project = Path(temporary)
        marker = "UNDERLYING_MARKER"
        (project / "fixture.txt").write_text((marker + " α界é\n") * 40)
        session = Session(binary, project, arguments=("fixture.txt",))
        try:
            session.send(b"\x1bOPSa")
            session.wait(b"Command: Sa")
            session.drain(.1)
            session.send(b"v")
            output = session.drain(.1)
            assert marker.encode() not in output, "prompt typing restored covered document"
            assert b"Save as" not in output, "unchanged menu row repainted"
            assert b"Command: Sav" in session.screen.text(), "incremental prompt text missing"
            session.send(b"\x1b[B")
            output = session.drain(.1)
            assert marker.encode() not in output, "arrow selection repainted document"
            # Shrinking/closing restores original Unicode cells and text.
            session.send(b"\x1b")
            session.drain(.1)
            assert (marker + " α界é").encode() in session.screen.text(), "overlay restore damaged UTF-8"
            session.send(b"\x1bOPSa")
            session.wait(b"Command: Sa")
            fcntl.ioctl(session.master, termios.TIOCSWINSZ, struct.pack("HHHH", 16, 80, 0, 0))
            session.screen = TerminalScreen(16, 80)
            session.process.send_signal(signal.SIGWINCH)
            session.wait(b"Command: Sa")  # resize redraws without an input key
            session.send(b"\x1b")
            session.drain(.1)
            assert marker.encode() in session.screen.text(), "resize/close did not restore editor"
        finally:
            session.close()
        print("Incremental overlay, Unicode restore and idle resize checks passed")


def screen_rowcol(screen, marker):
    """1-based SGR row/column of the first cell containing marker."""
    for row, line in enumerate(screen.lines):
        text = "".join(line)
        if marker in text:
            return row + 1, text.index(marker) + 1
    raise AssertionError(f"{marker!r} not on screen: {screen.text()[-300:]!r}")


def wait_until(predicate, timeout=5):
    """Poll a predicate so slow sanitizer builds do not need longer sleeps."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(.05)
    return predicate()


def wait_screen(session, marker, present=True, timeout=5):
    """Wait until marker is present (or gone) on the tracked screen."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        session.drain(.1)
        if (marker in session.screen.text()) == present:
            return True
    return (marker in session.screen.text()) == present


def wait_drain(session, predicate, timeout=5):
    """Wait for a predicate while draining the terminal, so slow builds settle."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        session.drain(.1)
        if predicate():
            return True
    return predicate()


def menu_mouse(binary, root):
    """Top bar, F10 menus and SGR mouse use real terminal reports."""
    with tempfile.TemporaryDirectory(prefix="xim-menu-mouse-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("hello world\nsecond line\nthird\n")
        (directory / "beta.txt").write_text("beta file\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            # Top bar is always visible and reserves the first row.
            assert b"File" in session.screen.text() and b"Help" in session.screen.text()
            # F10 opens File; arrows move; Enter runs; Escape dismisses.
            session.send(b"\x1b[21~")
            session.wait(b"New")
            assert b"Ctrl-N" in session.screen.text()
            session.send(b"\x1b")
            session.drain(.2)
            assert b"New" not in session.screen.text()
            # Ctrl-N creates a new buffer; F3 still finds next match.
            session.send(b"\x0e")
            session.wait(b"Xim")
            assert session.snapshot() == "\n"
            session.send(b"dirty")
            session.send(b"\x11")
            session.wait(b"Save changes")
            session.send(b"\x1b")
            session.send(b"\x01\x7f")
            # Redo stays on Ctrl-Y; F10 is menus, not redo.
            session.send(b"typed")
            assert "typed" in session.snapshot()
            session.send(b"\x1a")
            assert "typed" not in session.snapshot()
            session.send(b"\x19")
            assert "typed" in session.snapshot()
            # SGR click positions the caret; status shows the column.
            session.ex("call setline(1, ['hello world', 'second line', 'third'])|call cursor(1,1)")
            session.send(b"\x1b[<0;7;2M\x1b[<0;7;2m")
            session.wait(b"1:7")
            # Drag selects; replacement and copy work on the selection.
            session.send(b"\x1b[<0;2;2M")
            session.send(b"\x1b[<32;7;2M")
            session.send(b"\x1b[<0;7;2m")
            session.send(b"!")
            assert session.snapshot().startswith("h!world"), repr(session.snapshot()[:30])
            session.send(b"\x1a")
            # Double-click selects a word; copy then middle-click pastes it.
            session.ex("call setline(1, ['hello world', 'second line', 'third'])|call cursor(1,1)")
            session.drain(.6)  # let the previous multi-click state expire
            session.send(b"\x1b[<0;3;2M\x1b[<0;3;2m\x1b[<0;3;2M\x1b[<0;3;2m")
            session.drain(.2)
            session.send(b"\x03")
            session.send(b"\x1b[<1;1;3M\x1b[<1;1;3m")
            session.drain(.3)
            assert session.snapshot().splitlines()[1] == "hellosecond line", repr(session.snapshot())
            session.send(b"\x1a")
            # Triple-click selects the logical line; typing replaces it.
            session.ex("call setline(1, ['hello world', 'second line', 'third'])|call cursor(1,1)")
            session.drain(.6)
            session.send(b"\x1b[<0;3;2M\x1b[<0;3;2m\x1b[<0;3;2M\x1b[<0;3;2m\x1b[<0;3;2M\x1b[<0;3;2m")
            session.drain(.2)
            session.send(b"Z")
            assert session.snapshot().startswith("Z\nsecond"), repr(session.snapshot())
            session.send(b"\x1a")
            # Wheel keeps the caret; keyboard brings it back into view.
            session.ex("call setline(1, map(range(1,50), 'string(v:val)'))|call cursor(1,1)")
            session.send(b"\x1b[<65;50;10M")
            session.drain(.3)
            assert "".join(session.screen.lines[1]).strip() != "1", "wheel did not move the view"
            session.ex("call writefile([string(line('w0'))], 'top')")
            assert (directory / "top").read_text().strip() != "1", "wheel did not scroll"
            session.ex("call writefile([string(col('.'))], 'caret')")
            assert (directory / "caret").read_text().strip() == "1", "wheel moved caret"
            # Clicking a heading then an entry runs the shared action.
            session.send(b"\x1b[<0;2;1M\x1b[<0;2;1m")
            session.wait(b"Save as")
            row, column = screen_rowcol(session.screen, "Save as")
            session.send(f"\x1b[<0;{column};{row}M\x1b[<0;{column};{row}m".encode())
            session.wait(b"Save as:")
            session.send(b"\x1b")
            session.drain(.2)
            # Right-click opens Edit choices without clearing selection.
            session.send(b"\x01")
            session.send(b"\x1b[<2;50;12M\x1b[<2;50;12m")
            session.wait(b"Copy")
            session.send(b"\x1b")
            session.drain(.2)
            # Confirmation choices accept clicks; Cancel keeps editing.
            session.send(b"x")
            session.send(b"\x11")
            session.wait(b"Save changes")
            row, column = screen_rowcol(session.screen, "[Esc] Cancel")
            session.send(f"\x1b[<0;{column + 2};{row}M\x1b[<0;{column + 2};{row}m".encode())
            session.drain(.4)
            assert b"Save changes" not in session.screen.text()
            # Invalid coordinates never crash.
            session.send(b"\x1b[<0;999;999M\x1b[<0;999;999m")
            session.drain(.2)
            session.send(b"ok")
            assert "ok" in session.snapshot()
            # Split separators resize through the owning-thread adapters.
            session.ex("split")
            session.drain(.3)
            session.ex("call writefile([string(winheight(1)), string(winheight(2))], 'heights')")
            before_split = (directory / "heights").read_text().split()
            status_row, _ = screen_rowcol(session.screen, "Xim [No Name]")
            session.send(f"\x1b[<0;40;{status_row}M\x1b[<32;40;{status_row + 3}M\x1b[<0;40;{status_row + 3}m".encode())
            session.drain(.4)
            session.ex("call writefile([string(winheight(1)), string(winheight(2))], 'heights')")
            after_split = (directory / "heights").read_text().split()
            assert after_split != before_split, (before_split, after_split)
            session.ex("only")
            session.drain(.2)
            # A resize during pointer capture keeps the selection and does
            # not corrupt the editor.
            session.send(b"\x1b[<0;10;10M")
            session.screen = TerminalScreen(16, 80)
            session.process.send_signal(signal.SIGWINCH)
            session.drain(.4)
            session.send(b"\x1b[<0;10;10m")
            session.drain(.2)
            session.send(b"RESIZE")
            assert "RESIZE" in session.snapshot()
        finally:
            session.close()

        # Project root: picker rows and lists use the rendered layout.
        project = Session(binary, directory, arguments=(".",))
        try:
            project.send(b"\x10")
            project.wait(b"Files:")
            project.send(b"txt")
            project.wait(b"alpha.txt")
            project.wait(b"beta.txt")
            # Let the pending match settle so the click cannot race it.
            for _ in range(20):
                before = project.screen.text()
                project.drain(.1)
                if project.screen.text() == before:
                    break
            row, column = screen_rowcol(project.screen, "beta.txt")
            project.send(f"\x1b[<0;{column};{row}M\x1b[<0;{column};{row}m".encode())
            project.wait(b"beta file")
            assert project.snapshot().startswith("beta file"), repr(project.snapshot())
            # Wheel moves the palette list selection.
            project.send(b"\x1bOQ")
            project.wait(b"Command:")
            project.drain(.3)
            before = [line for line in project.screen.text().decode().splitlines() if line.startswith(">")]
            project.send(b"\x1b[<65;10;18M")
            project.drain(.3)
            after = [line for line in project.screen.text().decode().splitlines() if line.startswith(">")]
            assert before and after != before, (before, after)
            project.send(b"\x1b")
            project.drain(.2)
        finally:
            project.close()

        # Compatibility Vim keeps its inherited screen; no native bar.
        compat = Session(binary, directory, ("--vim", "alpha.txt"), ready=b"alpha.txt")
        try:
            compat.drain(.3)
            assert b"File  Edit" not in compat.screen.text(), "compatibility mode painted the menu bar"
            compat.send(b":qall!\r")
            compat.drain(.3)
        finally:
            compat.close()
        print("Menu bar, mouse editing, pickers and mode separation checks passed")


def capture_lifetime(binary, root):
    """A separator capture must not outlive the window it captured (R1)."""
    config = ("--cmd", "set t_u7= t_RB= t_RF= t_RV= t_RK=")
    with tempfile.TemporaryDirectory(prefix="xim-capture-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "a.txt").write_text("AAA\n")
        (directory / "b.txt").write_text("BBB\n")
        # Native Close (Ctrl-W) removes a window while a separator is captured.
        session = Session(binary, directory, config + ("a.txt",))
        try:
            session.ex("vsplit b.txt")
            session.drain(.2)
            session.send(b"\x1b[<0;51;5M")
            session.drain(.2)
            session.send(b"\x17")
            session.drain(.2)
            session.send(b"\x1b[<32;55;5M")
            session.drain(.3)
            assert session.process.poll() is None, "editor died after a mid-capture close"
            session.send(b"ok")
            assert "ok" in session.snapshot()
        finally:
            session.close()
        # Ex path: :only removes the captured window before the drag.
        session = Session(binary, directory, config + ("a.txt",))
        try:
            session.ex("split")
            session.drain(.2)
            session.send(b"\x1b[<0;40;5M")
            session.drain(.2)
            session.ex("wincmd j|only")
            session.drain(.2)
            session.send(b"\x1b[<32;40;8M")
            session.drain(.3)
            assert session.process.poll() is None, "editor died after :only during a capture"
            session.send(b"ok")
            assert "ok" in session.snapshot()
        finally:
            session.close()
    print("Separator capture lifetime checks passed")


def confirm_label_clicks(binary, root):
    """A click chooses the confirmation label it lands on (R2)."""
    def run(choice, label, expect_exit, expected_disk):
        with tempfile.TemporaryDirectory(prefix="xim-confirm-", dir=root) as temporary:
            directory = Path(temporary)
            (directory / "alpha.txt").write_text("old contents\n")
            session = Session(binary, directory, ("alpha.txt",))
            try:
                session.send(b"UNSAVED")
                session.send(b"\x11")
                session.wait(b"Save changes")
                session.drain(.2)
                row, column = screen_rowcol(session.screen, label)
                session.send(f"\x1b[<0;{column + 2};{row}M\x1b[<0;{column + 2};{row}m".encode())
                if expect_exit:
                    assert wait_until(lambda: session.process.poll() is not None), \
                        (choice, "editor did not exit")
                    assert (directory / "alpha.txt").read_text() == expected_disk, choice
                else:
                    session.drain(.4)
                    assert session.process.poll() is None, (choice, "editor exited")
                    session.send(b"Z")
                    assert "UNSAVEDZ" in session.snapshot(), choice
            finally:
                session.close()
    run("save", "[s] Save", True, "UNSAVEDold contents\n")
    run("discard", "[d] Discard", True, "old contents\n")
    run("cancel", "[Esc] Cancel", False, None)
    print("Confirmation label click checks passed")


def picker_unselected_click(binary, root):
    """Clicking an unselected picker row opens that row (R3)."""
    with tempfile.TemporaryDirectory(prefix="xim-picker-unsel-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("alpha beta\nsecond line\nthird\n")
        (directory / "beta.txt").write_text("BETA_ONLY\n")
        session = Session(binary, directory, arguments=(".",))
        try:
            session.send(b"\x10txt")
            session.wait(b"Files: txt")
            session.wait(b"beta.txt")
            for _ in range(20):
                before = session.screen.text()
                session.drain(.1)
                if session.screen.text() == before:
                    break
            rows = {}
            selected_name = None
            for name in ("alpha.txt", "beta.txt"):
                row, column = screen_rowcol(session.screen, name)
                rows[name] = (row, column)
                if session.screen.text().decode().splitlines()[row - 1].lstrip().startswith(">"):
                    selected_name = name
            assert selected_name is not None, session.screen.text()[-300:]
            other = "beta.txt" if selected_name == "alpha.txt" else "alpha.txt"
            row, column = rows[other]
            session.send(f"\x1b[<0;{column + 1};{row}M\x1b[<0;{column + 1};{row}m".encode())
            session.drain(.4)
            expected = "BETA_ONLY" if other == "beta.txt" else "alpha beta"
            assert session.snapshot().startswith(expected), (other, session.snapshot()[:40])
        finally:
            session.close()
    print("Unselected picker row click check passed")


def shift_click_placement(binary, root):
    """Shift-click must position and extend, not decode as a wheel (R4)."""
    with tempfile.TemporaryDirectory(prefix="xim-shift-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("alpha beta\nsecond line\nthird\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            # Shift-click with no selection places the caret at the click.
            session.send(b"\x1b[<4;7;2M\x1b[<4;7;2m")
            session.drain(.2)
            session.send(b"!")
            assert session.snapshot().startswith("alpha !beta"), repr(session.snapshot()[:20])
            # Shift-click with an existing selection extends it from the
            # anchor to the click instead of inserting at the line start.
            session.ex("call setline(1, 'abcdefghij')|call cursor(1,1)")
            session.drain(.5)
            session.send(b"\x1b[1;2C")
            session.drain(.1)
            session.send(b"\x1b[<4;5;2M\x1b[<4;5;2m")
            session.drain(.2)
            session.send(b"Z")
            assert session.snapshot().startswith("Zefghij"), repr(session.snapshot()[:20])
        finally:
            session.close()
    print("Shift-click placement and extension checks passed")


def new_buffer_dirty(binary, root):
    """New over a modified buffer saves it and keeps it listed (R5)."""
    with tempfile.TemporaryDirectory(prefix="xim-new-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("alpha\n")
        (directory / "beta.txt").write_text("beta\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            session.ex("badd beta.txt")
            session.send(b"X")
            session.send(b"\x0e")
            session.wait(b"Save changes")
            session.drain(.2)
            session.send(b"s")
            assert wait_until(lambda: (directory / "alpha.txt").read_text() == "Xalpha\n"), \
                "dirty buffer was not saved"
            session.ex("call writefile([expand('%:t'), string(bufnr('%')), "
                       "string(map(getbufinfo(), '[v:val.bufnr, v:val.name, v:val.listed]'))], 'state')")
            lines = (directory / "state").read_text().split("\n")
            assert lines[0] == "", ("New did not reach a fresh buffer", lines)
            assert "alpha.txt" in lines[2], ("saved buffer left the list", lines)
        finally:
            session.close()
    print("New over a modified buffer check passed")


def buffer_switch_and_about(binary, root):
    """Next buffer survives dirt; About does not raise E15 (R6)."""
    with tempfile.TemporaryDirectory(prefix="xim-switch-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("alpha\n")
        (directory / "beta.txt").write_text("beta\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            session.ex("badd beta.txt")
            session.send(b"DIRTY")
            # Navigate > Next buffer.
            session.send(b"\x1b[21~")
            session.wait(b"New")
            for _ in range(3):
                session.send(b"\x1b[C")
            session.drain(.2)
            for _ in range(2):
                session.send(b"\x1b[B")
            session.drain(.2)
            session.send(b"\r")
            session.ex("call writefile([expand('%:t')], 'cur')")
            assert (directory / "cur").read_text().strip() == "beta.txt", (directory / "cur").read_text()
            # Help > About.
            session.send(b"\x1b[21~")
            session.wait(b"New")
            for _ in range(4):
                session.send(b"\x1b[C")
            session.drain(.2)
            session.send(b"\x1b[B")
            session.drain(.2)
            session.send(b"\r")
            session.wait(b"one-run terminal editor")
            assert b"E15" not in session.screen.text(), "About raised E15"
        finally:
            session.close()
    print("Next buffer and About checks passed")


def split_wheel_caret(binary, root):
    """Wheel in a same-buffer split leaves both carets alone (R7)."""
    with tempfile.TemporaryDirectory(prefix="xim-split-wheel-", dir=root) as temporary:
        directory = Path(temporary)
        lines = "\n".join(f"line{i:03d} abcdefghijklmnopqrstuvwxyz" for i in range(1, 121))
        (directory / "alpha.txt").write_text(lines + "\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            session.ex("split")
            session.ex("call cursor(30,5)")
            session.ex("wincmd j")
            session.ex("call cursor(1,1)")
            session.drain(.3)
            command = "call writefile([string(getcurpos(1)), string(getcurpos(2))], 'pos')"
            session.ex(command)
            before = (directory / "pos").read_text()
            for row in (3, 20):
                session.send(f"\x1b[<65;20;{row}M".encode())
                session.drain(.4)
                session.ex(command)
                after = (directory / "pos").read_text()
                assert after == before, ("wheel moved a caret", row, before, after)
        finally:
            session.close()
    print("Same-buffer split wheel caret check passed")


def menu_overlay_cleanup(binary, root):
    """Menus restore short lines and clear over a live prompt (R8, N1)."""
    with tempfile.TemporaryDirectory(prefix="xim-menu-clean-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text("short\ntiny\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            session.ex("call setline(1, repeat(['ABCDEFGHIJKLMNOPQRSTUVWXYZ'], 30))")
            # Wait for the edit to render, then compare the rows the menu
            # covers; the message line keeps the Ex acknowledgement and is
            # not part of the artifact claim.
            assert wait_screen(session, b"ABCDEFGHIJKLMNOPQRSTUVWXYZ"), "setline did not render"
            def menu_region():
                return session.screen.text().decode().splitlines()[1:7]
            def check_dismiss(open_keys, marker):
                before = menu_region()
                session.send(open_keys)
                assert wait_screen(session, marker), "menu did not open"
                session.send(b"\x1b")
                assert wait_screen(session, marker, present=False), "menu survived Escape"
                assert wait_drain(session, lambda: menu_region() == before), \
                    "menu dismissal left artifacts"
            # The File menu has no disabled entries.
            check_dismiss(b"\x1b[21~", b"Quit")
            # The Edit drop-down renders " (disabled)" entries; its restore
            # rectangle must still cover every painted column.
            check_dismiss(b"\x1b[21~\x1b[C", b"(disabled)")
            # The right-click context menu renders the same disabled items.
            check_dismiss(b"\x1b[<2;5;2M\x1b[<2;5;2m", b"(disabled)")
            # A menu opened over a prompt is erased when it closes.
            session.send(b"\x0fabc")
            session.wait(b"Open: abc")
            session.drain(.2)
            session.send(b"\x1b[<0;2;1M\x1b[<0;2;1m")
            assert wait_screen(session, b"Quit"), "menu did not open over the prompt"
            session.send(b"\x1b")
            assert wait_screen(session, b"Quit", present=False), \
                "menu survived Escape over a prompt"
            session.send(b"\x1b")
            session.drain(.2)
        finally:
            session.close()
    print("Menu overlay cleanup checks passed")


def horizontal_wheel_direction(binary, root):
    """Wheel-left moves the view left and wheel-right right (R9)."""
    with tempfile.TemporaryDirectory(prefix="xim-hwheel-", dir=root) as temporary:
        directory = Path(temporary)
        (directory / "alpha.txt").write_text(("x" * 200) + "\nsecond\n")
        session = Session(binary, directory, ("alpha.txt",))
        try:
            session.ex("set nowrap")
            # Put the caret near the end so the view scrolls right and
            # 'leftcol' is non-zero before the wheel moves it.
            session.ex("call cursor(1, 200)")
            session.drain(.2)
            command = "call writefile([string(winsaveview().leftcol)], 'leftcol')"
            session.ex(command)
            before = int((directory / "leftcol").read_text().strip())
            session.send(b"\x1b[<66;20;10M")
            session.drain(.3)
            session.ex(command)
            left = int((directory / "leftcol").read_text().strip())
            assert left < before, ("wheel left should move the view left", before, left)
            session.send(b"\x1b[<67;20;10M")
            session.drain(.3)
            session.ex(command)
            right = int((directory / "leftcol").read_text().strip())
            assert right > left, ("wheel right should move the view right", left, right)
        finally:
            session.close()
    print("Horizontal wheel direction check passed")


def undo_redo_availability(binary, root):
    """Edit menu disables Undo/Redo with no history (R10)."""
    with tempfile.TemporaryDirectory(prefix="xim-undo-", dir=root) as temporary:
        directory = Path(temporary)
        session = Session(binary, directory)
        try:
            # Fresh buffer: neither Undo nor Redo can run.
            session.send(b"\x1b[21~")
            session.wait(b"New")
            session.send(b"\x1b[C")
            session.drain(.2)
            session.send(b"\x1b[B")
            session.drain(.2)
            session.send(b"\r")
            session.drain(.3)
            assert b"Already at newest change" not in session.screen.text()
            assert b"Already at oldest change" not in session.screen.text()
            # After a change Undo becomes reachable and works.
            session.send(b"abc")
            session.drain(.2)
            session.send(b"\x1b[21~")
            session.wait(b"New")
            session.send(b"\x1b[C")
            session.drain(.2)
            session.send(b"\r")
            session.drain(.4)
            assert session.snapshot() == "\n", "Undo did not run from the Edit menu"
        finally:
            session.close()
    print("Undo/redo availability checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--theme", type=Path, action="append", default=[])
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    run(args.binary.resolve(), args.work.resolve(), args.theme)
    project_run(args.binary.resolve(), args.work.resolve())
    project_gitignore(args.binary.resolve(), args.work.resolve())
    project_symlinks(args.binary.resolve(), args.work.resolve())
    project_explicit_path(args.binary.resolve(), args.work.resolve())
    project_single_file_no_scan(args.binary.resolve(), args.work.resolve())
    project_refresh_preserves_expansion(args.binary.resolve(), args.work.resolve())
    project_duplicate_basenames(args.binary.resolve(), args.work.resolve())
    project_gitignore_anchored(args.binary.resolve(), args.work.resolve())
    project_gitignore_scoped(args.binary.resolve(), args.work.resolve())
    project_picker_arrows(args.binary.resolve(), args.work.resolve())
    project_dirty_switch(args.binary.resolve(), args.work.resolve())
    project_background(args.binary.resolve(), args.work.resolve())
    ui_incremental(args.binary.resolve(), args.work.resolve())
    buffer_picker_identity(args.binary.resolve(), args.work.resolve())
    menu_mouse(args.binary.resolve(), args.work.resolve())
    capture_lifetime(args.binary.resolve(), args.work.resolve())
    confirm_label_clicks(args.binary.resolve(), args.work.resolve())
    picker_unselected_click(args.binary.resolve(), args.work.resolve())
    shift_click_placement(args.binary.resolve(), args.work.resolve())
    new_buffer_dirty(args.binary.resolve(), args.work.resolve())
    buffer_switch_and_about(args.binary.resolve(), args.work.resolve())
    split_wheel_caret(args.binary.resolve(), args.work.resolve())
    menu_overlay_cleanup(args.binary.resolve(), args.work.resolve())
    horizontal_wheel_direction(args.binary.resolve(), args.work.resolve())
    undo_redo_availability(args.binary.resolve(), args.work.resolve())
