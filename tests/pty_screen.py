"""pty_screen.py — a minimal ANSI screen emulator for the TUI pty tests.

Replays the byte stream a real terminal would receive onto a character grid so a
test can assert what the user would actually see (the in-memory test backend only
captures raw bytes). It implements the subset of sequences agentc emits:

  CR, LF (with scrolling), ESC[<n>A/B, ESC[<n>G, ESC[<r>;<c>H, ESC[2K, ESC[K,
  ESC[J (erase to end of screen),
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

Cursor models. A real xterm.js does not leave the cursor where the app parked
it across a resize: reflow moves it, so the app's cursor-relative erase no
longer starts at the owned region's top. Two opt-in modes model that drift (the
constructor defaults keep the pre-existing top-anchored behaviour byte for byte):

  - `cursor_drift=<rows>`: on `resize()`, move the cursor down by `rows` (clamped
to the last row). The inline composer parks the hidden cursor at the region top;
xterm.js was measured moving it down by the live region height-1, i.e. to the
region bottom, so `cursor_drift=live_rows-1` reproduces the stranded composer.
  - `cursor_to_bottom=True`: on `resize()`, move the cursor to the last row
(keeping its column). Equivalent to `cursor_drift` large enough to clamp;
useful when the caller does not want to track the region height.
  - `bottom_grow=True`: on a height GROW, shift the reflowed screen down by the
row delta and prepend blank rows, so bottom-flushed content (the inline composer
at the screen bottom) stays bottom-flushed. This is the xterm.js viewport law the
reported drift was measured against; the default leaves content top-anchored.
Note it changes the content layout, not just the cursor.
  - `width_bottom_flush=True` (with `cursor_landing="top"|"bottom"`): on a WIDTH
resize, keep the reflowed content bottom-flushed (the measured reporter law: after
any resize the region is bottom-flushed and a mid-screen region moves to the
bottom-flushed top) and let the parked cursor land on that region top (`"top"`)
or on the region bottom (`"bottom"`). Additive and off by default, so the control
and every existing model stay byte-identical.
  - `grow_top_anchor=True` (only meaningful with `width_bottom_flush`): model the
real terminal's GROW arm of the width law. A width SHRINK bottom-flushes (the
measured reporter law), but a width GROW needs no scroll, so the reflowed content
stays exactly where it was (top-anchored) and is NOT bottom-flushed. Without it,
`width_bottom_flush` blanket-flushes a GROW too, which moves an old-width region
down under the app's erase and MASKS a top-anchored grow bug: a regression test
could never see the stale band a real user sees. Additive and off by default, so
the control and every existing model stay byte-identical.
  - `hard_breaks=True`: an autowrap-faithful reflow model. The app writes every
  owned row with autowrap suppressed (`ESC[?7l`), so a row that fills the width is
  a HARD line and a real terminal never marks it as a soft-wrap continuation. The
  default emulator instead applies a tmux-style heuristic that marks any
  full-width row as a continuation, which is right for autowrap-on shell output
  but wrong for the app's rows; under that join model a width reflow can merge the
  app's hard rows and produce FEWER rows than the hard-wrap count. With
  `hard_breaks` the emulator tracks mode 7 and only marks a continuation when the
  row was written with autowrap on, so each app row re-wraps independently
  (`floor((w+cols-1)/cols)` rows) and the hard-break row count is exact. Additive
  and off by default: every existing model stays byte-identical.
  - `bottom_anchor=True`: model a real terminal's viewport, which is anchored at
  the bottom of the buffer. When a column reflow makes the grid taller than the
  screen, scroll the extra TOP rows into history and keep the bottom rows visible
  instead of discarding the bottom rows below the cursor. The measured reporter
  law (a width resize leaves the region bottom-flushed) needs this: on a narrow
  shrink the region's own rows reflow taller and a bottom-anchored terminal makes
  room above them. Additive and off by default, so every existing model keeps the
  keep-cursor-visible truncation byte for byte.
"""


class Screen:
    def __init__(self, cols, rows, cursor_drift=0, cursor_to_bottom=False,
                 bottom_grow=False, width_bottom_flush=False,
                 cursor_landing=None, hard_breaks=False, bottom_anchor=False,
                 grow_top_anchor=False):
        self.cols = max(cols, 1)
        self.rows = max(rows, 1)
        # xterm.js-style cursor drift across a resize (see the module docstring).
        # 0 / False preserve the original top-anchored emulator exactly.
        self.cursor_drift = max(0, int(cursor_drift))
        self.cursor_to_bottom = bool(cursor_to_bottom)
        # On a height grow, keep bottom-flushed content at the bottom (xterm.js
        # viewport). False preserves the original top-anchored emulator exactly.
        self.bottom_grow = bool(bottom_grow)
        # Measured width-change law (additive, opt-in): on a width resize the
        # reflowed content is bottom-flushed and the parked cursor lands on the
        # region top or bottom. False/None preserve every existing model.
        self.width_bottom_flush = bool(width_bottom_flush)
        self.cursor_landing = cursor_landing
        # Real-terminal GROW arm of the width law (see the module docstring):
        # a width GROW stays top-anchored instead of being bottom-flushed.
        # Off by default; only modifies the `width_bottom_flush` width block.
        self.grow_top_anchor = bool(grow_top_anchor)
        # Autowrap-faithful reflow (see the module docstring). Off by default.
        self.hard_breaks = bool(hard_breaks)
        self.autowrap = True
        # Bottom-anchored viewport reflow (see the module docstring).
        self.bottom_anchor = bool(bottom_anchor)
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
        # This is the heuristic tmux uses for rows written without autowrap. In
        # hard_breaks mode the app's `ESC[?7l` rows are hard lines, so only a row
        # written with autowrap ON can be a continuation.
        full = len("".join(self.grid[self.row]).rstrip()) >= self.cols
        if self.hard_breaks and not self.autowrap:
            full = False
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
                if mode == 7:
                    self.autowrap = on
                    if on:                   # autowrap on: clear pending wrap
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
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2:
                for r in range(self.rows):
                    self.grid[r] = [" "] * self.cols
                    self.wrapped[r] = False
            elif mode == 0:
                for x in range(self.col, self.cols):
                    self.grid[self.row][x] = " "
                for r in range(self.row + 1, self.rows):
                    self.grid[r] = [" "] * self.cols
                    self.wrapped[r] = False
            else:   # mode 1: start of screen to the cursor
                for r in range(0, self.row):
                    self.grid[r] = [" "] * self.cols
                    self.wrapped[r] = False
                for x in range(0, min(self.col + 1, self.cols)):
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
        old_cols = self.cols
        old_rows = self.rows
        grid_text = ["".join(r) for r in self.grid]

        sb_rows, sb_flags, _ = self._reflow(
            self.scrollback, self.sb_wrapped, cols, None, 0)
        grid_rows, grid_flags, cur = self._reflow(
            grid_text, self.wrapped, cols, self.row, self.col)
        cur_row = cur[0] if cur else rows - 1
        cur_col = cur[1] if cur else cols - 1

        # Keep the cursor visible: rows above it scroll into history until it is
        # on screen, and rows below the new bottom are discarded. A
        # bottom-anchored viewport instead keeps the reflowed content's bottom
        # visible and scrolls the extra TOP rows into history (a real terminal's
        # scroll position is measured from the bottom, so the app's bottom-flushed
        # region makes room above itself instead of losing its own tail).
        # Faithful (xterm.js/tmux) bottom-anchored viewport: trailing blank pad
        # rows are NOT content, so they never trigger a scroll. Only reflowed
        # *content* taller than the screen pushes its TOP rows into history;
        # otherwise the content stays at row 1 with blanks below (baseY=0).
        # Measured against xterm.js 6.1.0-beta.288.
        if self.bottom_anchor:
            last = -1
            for i in range(len(grid_rows) - 1, -1, -1):
                if grid_rows[i].strip():
                    last = i
                    break
            content = grid_rows[:last + 1]
            cflags = grid_flags[:last + 1]
            overflow = len(content) - rows
            if overflow > 0:
                sb_rows += content[:overflow]
                sb_flags += cflags[:overflow]
                grid_rows = content[overflow:]
                grid_flags = cflags[overflow:]
                cur_row -= overflow
            else:
                grid_rows = content
                grid_flags = cflags
            if cur_row < 0:
                cur_row = 0
        else:
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
        # bottom-anchored grow: the viewport keeps bottom-flushed content at the
        # bottom, so the new rows appear above it. Shift the reflowed screen down
        # by the row delta and prepend blanks; the cursor rides down with it. The
        # default (bottom_grow=False) leaves content top-anchored, byte-identical
        # to the pre-existing emulator.
        if self.bottom_grow and rows > old_rows:
            delta = rows - old_rows
            self.grid = [[" "] * cols for _ in range(delta)] + self.grid[:rows - delta]
            self.wrapped = [False] * delta + self.wrapped[:rows - delta]
            self.row = min(rows - 1, self.row + delta)
        # xterm.js reflow moves the parked cursor instead of leaving it at the
        # region top the app assumes. Model that move last, so it is what the
        # app's post-resize cursor-relative erase is replayed against.
        if self.cursor_to_bottom:
            self.row = rows - 1
        elif self.cursor_drift:
            self.row = min(rows - 1, self.row + self.cursor_drift)
        # The measured width law is TOP-ANCHORED: content stays at row 1 with
        # blanks below; the old `width_bottom_flush` down-shift (blank rows above
        # content) is not realizable and was removed (N44 reflow measurement).
        # `width_bottom_flush`/`cursor_landing`/`grow_top_anchor` are kept as
        # accepted-but-inert attributes so legacy scenarios still construct.
        return self
