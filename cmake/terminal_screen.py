"""Small incremental VT screen for PTY assertions, not a terminal implementation.

Keep rendered text across read boundaries: a cell-diff renderer need not emit
an unchanged prompt prefix again. Supports the sequences used by our fixtures.
"""
import codecs
import re
import unicodedata


class TerminalScreen:
    def __init__(self, rows=24, columns=100):
        self.rows, self.columns = rows, columns
        self.lines = [[" "] * columns for _ in range(rows)]
        self.row = self.col = 0
        self.top, self.bottom = 0, rows - 1
        self.saved = (0, 0)
        self.pending = ""
        self.decoder = codecs.getincrementaldecoder("utf-8")("replace")

    def text(self):
        return "\n".join("".join(line).rstrip() for line in self.lines).encode()

    def feed(self, data):
        text = self.pending + self.decoder.decode(data)
        self.pending = ""
        i = 0
        while i < len(text):
            c = text[i]
            if c == "\x1b":
                if i + 1 >= len(text):
                    self.pending = text[i:]
                    break
                lead = text[i + 1]
                if lead == "[":
                    match = re.match(r"\x1b\[([0-?]*)([ -/]*)([@-~])", text[i:])
                    if not match:
                        self.pending = text[i:]
                        break
                    params, _, command = match.groups()
                    private = params.startswith("?")
                    numbers = [int(p or "0") for p in params.lstrip("?=>").split(";")]
                    n = numbers[0] or 1
                    if not private:
                        if command in "Hf":
                            self.row = min(self.rows - 1, n - 1)
                            self.col = min(self.columns - 1, (numbers[1] or 1) - 1 if len(numbers) > 1 else 0)
                        elif command == "A": self.row = max(0, self.row - n)
                        elif command == "B": self.row = min(self.rows - 1, self.row + n)
                        elif command == "C": self.col = min(self.columns - 1, self.col + n)
                        elif command == "D": self.col = max(0, self.col - n)
                        elif command == "G": self.col = min(self.columns - 1, n - 1)
                        elif command == "d": self.row = min(self.rows - 1, n - 1)
                        elif command == "K":
                            start = 0 if numbers[0] in (1, 2) else self.col
                            end = self.col + 1 if numbers[0] == 1 else self.columns
                            self.lines[self.row][start:end] = [" "] * (end - start)
                        elif command == "J":
                            if numbers[0] == 2: self.lines = [[" "] * self.columns for _ in range(self.rows)]
                            elif numbers[0] == 0:
                                self.lines[self.row][self.col:] = [" "] * (self.columns - self.col)
                                for r in range(self.row + 1, self.rows): self.lines[r] = [" "] * self.columns
                        elif command == "r":
                            self.top = n - 1
                            self.bottom = (numbers[1] or self.rows) - 1 if len(numbers) > 1 else self.rows - 1
                            self.row = self.col = 0
                        elif command == "s": self.saved = (self.row, self.col)
                        elif command == "u": self.row, self.col = self.saved
                    i += len(match[0])
                    continue
                if lead in "]P":
                    end = text.find("\x07", i + 2)
                    st = text.find("\x1b\\", i + 2)
                    if end < 0 or (0 <= st < end): end = st
                    if end < 0:
                        self.pending = text[i:]
                        break
                    i = end + (2 if text[end] == "\x1b" else 1)
                    continue
                if lead in "()":
                    if i + 2 >= len(text):
                        self.pending = text[i:]
                        break
                    i += 3
                    continue
                if lead == "7": self.saved = (self.row, self.col)
                elif lead == "8": self.row, self.col = self.saved
                i += 2
                continue
            if c == "\r": self.col = 0
            elif c == "\n":
                if self.row == self.bottom:
                    del self.lines[self.top]
                    self.lines.insert(self.bottom, [" "] * self.columns)
                else: self.row = min(self.rows - 1, self.row + 1)
            elif c == "\b": self.col = max(0, self.col - 1)
            elif c == "\t": self.col = min(self.columns - 1, (self.col // 8 + 1) * 8)
            elif ord(c) >= 32:
                if unicodedata.combining(c):
                    if self.col: self.lines[self.row][self.col - 1] += c
                else:
                    width = 2 if unicodedata.east_asian_width(c) in "WF" else 1
                    self.lines[self.row][self.col] = c
                    if width == 2 and self.col + 1 < self.columns: self.lines[self.row][self.col + 1] = ""
                    self.col = min(self.columns - 1, self.col + width)
            i += 1
