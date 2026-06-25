"""Streaming D&D Beyond HP extractor.

The v5 character document is ~50-100 KB. On a 520 KB microcontroller, with a TLS
session already holding ~40-60 KB of buffers, we cannot afford to buffer or fully
parse it. So instead of ``json.load`` (which builds the whole dict), this module
runs an incremental, byte-at-a-time JSON scanner that keeps only:

  - a small path stack (key names / array indices of the containers we're inside),
  - the handful of scalar fields the HP formula needs,
  - at most one in-flight "element" accumulator for stats / modifier entries.

Long string values we don't care about (descriptions, notes) are scanned past
without being stored, so peak memory stays flat regardless of document size.

The HP *formula* (``compute_hp``) is a faithful port of ``internal/ddb/hp.go``:

    max     = overrideHitPoints, if set and >0; otherwise
              baseHitPoints + bonusHitPoints + conModifier*totalLevel
    current = clamp(max - removedHitPoints, 0, max)
    temp    = max(0, temporaryHitPoints)

where conModifier = floor((conScore - 10) / 2), conScore resolving base stat,
override (replaces), bonus (adds), and constitution-score modifiers (add).
"""

CON_STAT_ID = 3  # D&D Beyond ability-score id for Constitution

_HP_TOP = (
    "baseHitPoints",
    "bonusHitPoints",
    "overrideHitPoints",
    "removedHitPoints",
    "temporaryHitPoints",
)

# byte constants
_WS = b" \t\r\n"


def compute_hp(base, bonus, override, removed, temp, con_score, level):
    """Pure HP formula, mirroring computeHP() in hp.go. Returns (cur, max, temp)."""
    con_mod = (con_score - 10) // 2  # Python // already floors toward -inf
    if override is not None and override > 0:
        mx = override
    else:
        mx = base + bonus + con_mod * level
    if mx < 1:
        mx = 1  # a living character has >= 1 max HP
    cur = mx - removed
    if cur < 0:
        cur = 0
    elif cur > mx:
        cur = mx
    if temp < 0:
        temp = 0
    return cur, mx, temp


class _Scanner:
    """Incremental JSON scanner that captures only HP-relevant fields."""

    # parser states
    _VALUE = 0    # expecting a value
    _OBJKEY = 1   # expecting a key string or '}'
    _COLON = 2    # expecting ':'
    _OBJSEP = 3   # expecting ',' or '}'
    _ARRSEP = 4   # expecting ',' or ']'

    def __init__(self):
        self.state = self._VALUE
        self.kinds = []        # 'O' / 'A' per open container
        self.curs = []         # current child key (O) or index (A) per container
        self.cpaths = []       # path-tuple each container occupies (for close)
        self._bump = False     # next array value starts a new element index

        # in-flight token
        self._mode = None      # None | 'str' | 'num' | 'lit'
        self._buf = bytearray()
        self._capture = True   # whether to store the current string's bytes
        self._is_key = False
        self._esc = False
        self._uskip = 0

        # element accumulator (stats / modifier entries)
        self.elem = None       # dict of captured child scalars
        self.elem_path = None
        self.elem_kind = None  # 'stats' | 'bonusStats' | 'overrideStats' | 'mod'

        # captured HP fields
        self.fields = {}       # baseHitPoints etc.
        self.level = 0
        self.stat_con = None
        self.override_con = None
        self.bonus_con = None
        self.mod_con = 0
        self.data_null = False
        self.saw_any = False

        self.done = False      # set once we can stop reading early

    # -- public ------------------------------------------------------------

    def feed(self, chunk):
        i, n = 0, len(chunk)
        while i < n and not self.done:
            c = chunk[i]
            if self._mode == "str":
                i = self._feed_str(chunk, i, n)
                continue
            if self._mode in ("num", "lit"):
                # terminate token on a delimiter; reprocess that delimiter
                if c in b"{}[]:,\" \t\r\n":
                    self._finish_token()
                    continue
                self._buf.append(c)
                i += 1
                continue
            # between tokens
            if c in _WS:
                i += 1
                continue
            i = self._feed_struct(c, i)
        return self.done

    def finalize(self):
        if self._mode in ("num", "lit"):
            self._finish_token()
        if not self.saw_any:
            raise ValueError("ddb: no HP fields found (private sheet or bad response?)")
        con = 10
        if self.stat_con is not None:
            con = self.stat_con
        if self.override_con is not None:
            con = self.override_con
        if self.bonus_con is not None:
            con += self.bonus_con
        con += self.mod_con
        return compute_hp(
            self.fields.get("baseHitPoints", 0) or 0,
            self.fields.get("bonusHitPoints", 0) or 0,
            self.fields.get("overrideHitPoints", None),
            self.fields.get("removedHitPoints", 0) or 0,
            self.fields.get("temporaryHitPoints", 0) or 0,
            con,
            self.level,
        )

    # -- structural handling ----------------------------------------------

    def _feed_struct(self, c, i):
        st = self.state
        if st == self._VALUE:
            return self._begin_value(c, i)
        if st == self._OBJKEY:
            if c == 0x7D:  # '}'
                self._close("O")
                return i + 1
            if c == 0x22:  # '"'
                self._start_string(is_key=True, capture=True)
                return i + 1
            return i + 1  # tolerate stray chars
        if st == self._COLON:
            if c == 0x3A:  # ':'
                self.state = self._VALUE
            return i + 1
        if st == self._OBJSEP:
            if c == 0x2C:  # ','
                self.state = self._OBJKEY
            elif c == 0x7D:  # '}'
                self._close("O")
            return i + 1
        if st == self._ARRSEP:
            if c == 0x2C:  # ','
                self.state = self._VALUE
                self._bump = True
            elif c == 0x5D:  # ']'
                self._close("A")
            return i + 1
        return i + 1

    def _begin_value(self, c, i):
        # entering an array element: advance its index
        if self._bump and self.kinds and self.kinds[-1] == "A":
            self.curs[-1] += 1
            self._bump = False
        if c == 0x7B:  # '{'
            self._open("O")
            self.state = self._OBJKEY
            return i + 1
        if c == 0x5B:  # '['
            self._open("A")
            self.state = self._VALUE
            self.curs[-1] = -1
            self._bump = True
            return i + 1
        if c == 0x22:  # '"'
            self._start_string(is_key=False, capture=self._want_string())
            return i + 1
        if c == 0x2D or 0x30 <= c <= 0x39:  # '-' or digit
            self._mode = "num"
            self._buf = bytearray([c])
            return i + 1
        if c in b"tfn":  # true / false / null
            self._mode = "lit"
            self._buf = bytearray([c])
            return i + 1
        return i + 1

    def _open(self, kind):
        cpath = tuple(self.curs)
        self.kinds.append(kind)
        self.curs.append(None if kind == "O" else -1)
        self.cpaths.append(cpath)
        if kind == "O":
            self._on_open_object(cpath)

    def _close(self, kind):
        self.kinds.pop()
        self.curs.pop()
        cpath = self.cpaths.pop()
        if kind == "O":
            self._on_close_object(cpath)
        self._after_value()

    def _after_value(self):
        """Set the next state after a value/container completes."""
        if not self.kinds:
            self.done = True  # finished the top-level document
            return
        self.state = self._OBJSEP if self.kinds[-1] == "O" else self._ARRSEP

    # -- string handling ---------------------------------------------------

    def _start_string(self, is_key, capture):
        self._mode = "str"
        self._buf = bytearray()
        self._is_key = is_key
        self._capture = capture
        self._esc = False
        self._uskip = 0

    def _feed_str(self, chunk, i, n):
        while i < n:
            c = chunk[i]
            i += 1
            if self._uskip:
                self._uskip -= 1
                continue
            if self._esc:
                self._esc = False
                if c == 0x75:  # 'u' -> skip 4 hex (we never need unicode content)
                    self._uskip = 4
                elif self._capture:
                    self._buf.append(c)
                continue
            if c == 0x5C:  # backslash
                self._esc = True
                continue
            if c == 0x22:  # closing quote
                self._finish_string()
                return i
            if self._capture:
                self._buf.append(c)
        return i

    def _finish_string(self):
        s = self._buf.decode("utf-8") if self._capture else ""
        self._mode = None
        self._buf = bytearray()
        if self._is_key:
            self.curs[-1] = s
            self.state = self._COLON
        else:
            self._scalar(tuple(self.curs), s)
            self._after_value()

    # -- number / literal handling ----------------------------------------

    def _finish_token(self):
        text = bytes(self._buf)
        self._mode = None
        self._buf = bytearray()
        val = self._parse_scalar(text)
        self._scalar(tuple(self.curs), val)
        self._after_value()

    @staticmethod
    def _parse_scalar(text):
        if text == b"true":
            return True
        if text == b"false":
            return False
        if text == b"null":
            return None
        try:
            if b"." in text or b"e" in text or b"E" in text:
                return float(text)
            return int(text)
        except ValueError:
            return None

    # -- capture decisions -------------------------------------------------

    def _want_string(self):
        """Only modifier type/subType string values are worth storing."""
        if self.elem_kind == "mod" and self.elem_path is not None:
            cur = self.curs[-1]
            return cur in ("type", "subType")
        return False

    def _on_open_object(self, cpath):
        ln = len(cpath)
        if ln >= 1 and cpath[0] != "data":
            return
        if ln == 3 and cpath[1] in ("stats", "bonusStats", "overrideStats"):
            self.elem = {}
            self.elem_path = cpath
            self.elem_kind = cpath[1]
        elif ln == 4 and cpath[1] == "modifiers":
            self.elem = {}
            self.elem_path = cpath
            self.elem_kind = "mod"

    def _on_close_object(self, cpath):
        if cpath != self.elem_path:
            return
        e, kind = self.elem, self.elem_kind
        self.elem = None
        self.elem_path = None
        self.elem_kind = None
        if kind in ("stats", "bonusStats", "overrideStats"):
            if e.get("id") == CON_STAT_ID and e.get("value") is not None:
                v = e["value"]
                if kind == "stats":
                    self.stat_con = v
                elif kind == "overrideStats":
                    self.override_con = v
                else:
                    self.bonus_con = v
        elif kind == "mod":
            if e.get("type") == "bonus" and e.get("subType") == "constitution-score":
                v = e.get("value")
                if isinstance(v, int):
                    self.mod_con += v

    def _scalar(self, path, value):
        ln = len(path)
        # top-level HP fields under "data"
        if ln == 2 and path[0] == "data":
            k = path[1]
            if k in _HP_TOP:
                self.fields[k] = value
                self.saw_any = True
                self._maybe_done()
            return
        # data == null -> private sheet / failure
        if ln == 1 and path[0] == "data" and value is None:
            self.data_null = True
            return
        # class levels
        if ln == 4 and path[0] == "data" and path[1] == "classes" and path[3] == "level":
            if isinstance(value, int):
                self.level += value
            return
        # element children (stats id/value, modifier type/subType/value)
        if self.elem_path is not None and ln == len(self.elem_path) + 1 \
                and path[: len(self.elem_path)] == self.elem_path:
            self.elem[path[-1]] = value

    def _maybe_done(self):
        """If an override HP is set we don't need stats/classes/modifiers."""
        ov = self.fields.get("overrideHitPoints")
        if isinstance(ov, int) and ov > 0 \
                and "removedHitPoints" in self.fields \
                and "temporaryHitPoints" in self.fields:
            self.done = True


def parse_hp_stream(read, chunk_size=512):
    """Drive the scanner from a read callable (e.g. ssl socket .read).

    ``read(n)`` must return up to n bytes, or b"" / None at EOF.
    Returns (current, max, temp).
    """
    sc = _Scanner()
    while not sc.done:
        data = read(chunk_size)
        if not data:
            break
        sc.feed(data)
    return sc.finalize()


def parse_hp_bytes(data):
    """Parse a complete in-memory JSON body. Used by the desktop tests."""
    sc = _Scanner()
    sc.feed(data if isinstance(data, (bytes, bytearray)) else data.encode())
    return sc.finalize()
