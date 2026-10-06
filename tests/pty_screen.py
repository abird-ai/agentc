"""pty_screen.py — a minimal ANSI screen emulator for the TUI pty tests.

Replays the byte stream a real terminal would receive onto a character grid so a
test can assert what the user would actually see (the in-memory test backend only
captures raw bytes). It implements the subset of sequences agentc emits:

  CR, LF (with scrolling), ESC[<n>A/B, ESC[<n>G, ESC[<r>;<c>H, ESC[2K, ESC[K,
  SGR sequences (ignored), private modes (?7, ?25, ?1049, ?2004, ?2026).

Rows pushed off the top by scrolling are retained in `scrollback` (plain text,
oldest first). `resize()` models a reflowing terminal: a line feed after a
full-width line marks the next row as a soft-wrapped continuation, and resizing
joins continuations back into one logical line and re-wraps it at the new width
(this is what tmux/kitty/wezterm/VTE do, and what lets a narrowed terminal grow
the line count and a widened terminal shrink it again). Rows that no longer fit
move into scrollback; the cursor stays on the same character when it can.

`text()` stays the visible screen for existing callers; `all_text()` is
scrollback + screen. UTF-8 is decoded per codepoint (one box-drawing glyph lands
in one cell); wide glyphs are not given two cells, because the assertions here
only involve the narrow composer rules and ASCII.
"""


class Screen:
    def __init__(self, cols, rows):
        self.cols = max(cols, 1)
        self.rows = max(rows, 1)
        self.grid = [[" "] * self.cols for _ in range(self.rows)]
        # wrapped[r] is True when row r is a soft-wrap continuation of row r-1.
        self.wrapped = [False] * self.rows
        self.row = 0
        self.col = 0
        self.alt = False
        self.scrolled = 0        # lines pushed off the top
        self.scrollback = []     # plain strings, oldest first
        self.sb_wrapped = []     # matching continuation flags

    # ------------------------------------------------------------- engine

    def _clamp(self):
        if self.row < 0:
            self.row = 0
        if self.col < 0:
            self.col = 0
        if self.col >= self.cols:
            self.col = self.cols - 1
        if self.row >= self.rows:
            self.row = self.rows - 1

    def _push_top_row(self):
        self.scrollback.append("".join(self.grid[0]).rstrip())
        self.sb_wrapped.append(self.wrapped[0])
        self.grid.pop(0)
        self.wrapped.pop(0)
        self.grid.append([" "] * self.cols)
        self.wrapped.append(False)
        self.scrolled += 1

    def _lf(self):
        # A line that filled the width is a soft wrap; the row below continues it.
        # This is the heuristic tmux uses for rows written without autowrap.
        full = len("".join(self.grid[self.row]).rstrip()) >= self.cols
        self.row += 1
        if self.row >= self.rows:
            self._push_top_row()
            self.row = self.rows - 1
        self.wrapped[self.row] = full

    def _put(self, ch):
        if ch == "\n":
            self._lf()
            return
        if ch == "\r":
            self.col = 0
            return
        if ch == "\b":
            self.col = max(0, self.col - 1)
            return
        self._clamp()
        self.grid[self.row][self.col] = ch
        self.col += 1

    def feed(self, data):
        if isinstance(data, str):
            data = data.encode("latin-1", "replace")
        i = 0
        n = len(data)
        while i < n:
            b = data[i]
            if b == 0x1B:
                i = self._escape(data, i + 1)
                continue
            if b < 0x80:
                self._put(chr(b))
                i += 1
                continue
            # Decode one UTF-8 sequence so U+2500 occupies one cell, which the
            # composer-frame assertions depend on.
            if 0xC0 <= b < 0xE0:
                need = 2
            elif 0xE0 <= b < 0xF0:
                need = 3
            elif 0xF0 <= b < 0xF8:
                need = 4
            else:
                need = 1
            chunk = data[i:i + need]
            try:
                ch = chunk.decode("utf-8")
            except UnicodeDecodeError:
                ch = "\ufffd"
                need = 1
            self._put(ch)
            i += need
        return self

    def _escape(self, data, i):
        n = len(data)
        if i >= n:
            return n
        c = data[i]
        if c != ord("["):
            return i + 1                     # single-character escape: ignore
        i += 1
        private = False
        if i < n and data[i] == ord("?"):
            private = True
            i += 1
        params = ""
        while i < n and chr(data[i]) in "0123456789;":
            params += chr(data[i])
            i += 1
        if i >= n:
            return n
        final = chr(data[i])
        i += 1
        nums = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []
        if private:
            if final in "hl":
                on = final == "h"
                mode = nums[0] if nums else 0
                if mode == 1049:
                    self.alt = on
                if mode == 7 and on:         # autowrap on: clear pending wrap
                    self.col = min(self.col, self.cols - 1)
            return i
        if final == "A":
            self.row -= max(1, nums[0]) if nums else 1
        elif final == "B":
            self.row += max(1, nums[0]) if nums else 1
        elif final == "C":
            self.col += max(1, nums[0]) if nums else 1
        elif final == "D":
            self.col -= max(1, nums[0]) if nums else 1
        elif final == "G":
            self.col = (max(1, nums[0]) if nums else 1) - 1
        elif final == "H" or final == "f":
            r = (nums[0] if len(nums) > 0 and nums[0] else 1) - 1
            ccol = (nums[1] if len(nums) > 1 and nums[1] else 1) - 1
            self.row, self.col = r, ccol
            if 0 <= self.row < self.rows:
                self.wrapped[self.row] = False   # the row is about to be rewritten
        elif final == "K":
            mode = nums[0] if nums else 0
            if mode == 2:
                self.grid[self.row] = [" "] * self.cols
                self.wrapped[self.row] = False
            elif mode == 0:
                for x in range(self.col, self.cols):
                    self.grid[self.row][x] = " "
            else:
                for x in range(0, self.col):
                    self.grid[self.row][x] = " "
        # SGR (m) and anything else: no visual effect here
        self._clamp()
        return i

    # ------------------------------------------------------------- queries

    def text(self):
        return "\n".join("".join(r).rstrip() for r in self.grid)

    def scrollback_text(self):
        return "\n".join(self.scrollback)

    def all_text(self):
        """Scrollback followed by the visible screen."""
        parts = self.scrollback + ["".join(r).rstrip() for r in self.grid]
        return "\n".join(parts)

    def count_rows_containing(self, needle):
        return sum(1 for r in self.grid if needle in "".join(r))

    def count_scrollback_rows_containing(self, needle):
        return sum(1 for r in self.scrollback if needle in r)

    def count_all_rows_containing(self, needle):
        return self.count_scrollback_rows_containing(needle) + \
            self.count_rows_containing(needle)

    # ------------------------------------------------------------- resize

    @staticmethod
    def _wrap_logical(text, cols):
        """Wrap one logical line at `cols`; return (chunks, continuation flags)."""
        text = text.rstrip(" ")
        if not text:
            return [""], [False]
        chunks = [text[i:i + cols] for i in range(0, len(text), cols)]
        return chunks, [False] + [True] * (len(chunks) - 1)

    @classmethod
    def _reflow(cls, rows, flags, cols, cur_row, cur_col):
        """Join soft-wrapped rows into logical lines and re-wrap them. Returns
        (new_rows, new_flags, new_cursor or None)."""
        out = []
        out_flags = []
        cur = None
        i = 0
        while i < len(rows):
            text = rows[i]
            j = i
            while j + 1 < len(rows) and flags[j + 1]:
                j += 1
                text += rows[j]
            chunks, cf = cls._wrap_logical(text, cols)
            if cur is None and cur_row is not None and i <= cur_row <= j:
                off = cur_col + sum(len(rows[k]) for k in range(i, cur_row))
                ci = off // cols
                if ci >= len(chunks):
                    ci = len(chunks) - 1
                cur = (len(out) + ci, off - ci * cols)
            out += chunks
            out_flags += cf
            i = j + 1
        return out, out_flags, cur

    def resize(self, cols, rows):
        """Resize like a real terminal: history and screen are re-wrapped, then
        the cursor is kept visible. When the cursor would land below the new
        bottom, the rows above it move into scrollback and the rows below it are
        discarded (tmux/xterm shrink behaviour); otherwise content stays put and
        the screen is padded at the bottom."""
        cols = max(cols, 1)
        rows = max(rows, 1)
        grid_text = ["".join(r) for r in self.grid]

        sb_rows, sb_flags, _ = self._reflow(
            self.scrollback, self.sb_wrapped, cols, None, 0)
        grid_rows, grid_flags, cur = self._reflow(
            grid_text, self.wrapped, cols, self.row, self.col)
        cur_row = cur[0] if cur else rows - 1
        cur_col = cur[1] if cur else cols - 1

        # Keep the cursor visible: rows above it scroll into history until it is
        # on screen, and rows below the new bottom are discarded.
        shift = cur_row - (rows - 1)
        if shift > 0:
            sb_rows += grid_rows[:shift]
            sb_flags += grid_flags[:shift]
            grid_rows = grid_rows[shift:]
            grid_flags = grid_flags[shift:]
            cur_row -= shift
        grid_rows = grid_rows[:rows]
        grid_flags = grid_flags[:rows]
        while len(grid_rows) < rows:
            grid_rows.append("")
            grid_flags.append(False)

        self.scrollback = sb_rows
        self.sb_wrapped = sb_flags
        self.grid = [list(s.ljust(cols)) for s in grid_rows]
        self.wrapped = grid_flags
        self.cols, self.rows = cols, rows
        self.row = max(0, min(rows - 1, cur_row))
        self.col = max(0, min(cols - 1, cur_col))
        return self
