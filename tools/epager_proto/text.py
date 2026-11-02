"""Reference board text normalisation (protocol.md section 8).

Deliberately NOT unicodedata.normalize('NFC'): the board only composes Latin/Vietnamese with
a generated table, and this reference mirrors that algorithm exactly so the C code can be
checked byte-for-byte. tests/test_text.py checks it agrees with real NFC on Vietnamese.
"""
from __future__ import annotations

import unicodedata

from .registry import Registry, hexcp


class TextRules:
    def __init__(self, reg: Registry):
        t = reg.text
        self.placeholder = hexcp(t["placeholder"])
        self.ellipsis = hexcp(t["ellipsis"])
        self.mark_lo, self.mark_hi = (hexcp(x) for x in t["combining_range"])
        self.max_marks = t["max_combining_run"]
        self.ranges = [(hexcp(a), hexcp(b)) for a, b in t["renderable_ranges"]]
        self.removed = [(hexcp(a), hexcp(b)) for a, b in t["removed_ranges"]]
        self.compose_ranges = [(hexcp(a), hexcp(b)) for a, b in t["compose_ranges"]]
        self.emoji = [(hexcp(a), hexcp(b)) for a, b in t["emoji_ranges"]]
        self.zwj = hexcp(t["zwj"])
        self.ri_lo, self.ri_hi = (hexcp(x) for x in t["regional_indicator_range"])
        self.flag = hexcp(t["flag_glyph"])
        self.compose_table, self.ccc = build_tables(self)

    def is_mark(self, cp: int) -> bool:
        return self.mark_lo <= cp <= self.mark_hi

    def is_removed(self, cp: int) -> bool:
        return any(a <= cp <= b for a, b in self.removed)

    def is_emoji(self, cp: int) -> bool:
        return any(a <= cp <= b for a, b in self.emoji)

    def renderable(self, cp: int) -> bool:
        return any(a <= cp <= b for a, b in self.ranges) or self.is_emoji(cp)


def build_tables(rules: TextRules) -> tuple[dict[tuple[int, int], int], dict[int, int]]:
    """(base, mark) -> composed for every precomposed letter in compose_ranges (renderable or
    not: intermediate compositions such as U+0228 are needed to reach renderable ones), plus
    mark -> ccc."""
    table: dict[tuple[int, int], int] = {}
    for a, b in rules.compose_ranges:
        for cp in range(a, b + 1):
            d = unicodedata.decomposition(chr(cp))
            if not d or d.startswith("<"):
                continue
            parts = [int(x, 16) for x in d.split()]
            if len(parts) != 2 or not rules.is_mark(parts[1]):
                continue
            if unicodedata.normalize("NFC", chr(parts[0]) + chr(parts[1])) == chr(cp):
                table[(parts[0], parts[1])] = cp
    ccc = {cp: unicodedata.combining(chr(cp)) for cp in range(rules.mark_lo, rules.mark_hi + 1)}
    return table, ccc


_RULES: dict[int, TextRules] = {}


def rules(reg: Registry) -> TextRules:
    """TextRules cached per registry object."""
    if id(reg) not in _RULES:
        _RULES[id(reg)] = TextRules(reg)
    return _RULES[id(reg)]


def compose(r: TextRules, base: int, marks: list[int]) -> tuple[int, list[int]]:
    """Canonical composition of one cluster (marks already capped)."""
    marks = sorted(marks, key=lambda m: r.ccc.get(m, 0))  # stable: canonical ordering
    left: list[int] = []
    for m in marks:
        c = r.ccc.get(m, 0)
        blocked = any(r.ccc.get(x, 0) == 0 or r.ccc.get(x, 0) >= c for x in left)
        composed = None if blocked else r.compose_table.get((base, m))
        if composed is None:
            left.append(m)
        else:
            base = composed
    return base, left


def normalize(reg: Registry, data: bytes, budget: int | None = None) -> str:
    r = rules(reg)
    s = data.decode("utf-8", errors="replace")

    # 1-2: controls, then emoji sequences collapse to their first emoji: a ZWJ drops the next emoji,
    # a pair of regional indicators becomes one flag glyph
    cps: list[int] = []
    after_zwj = False
    ri_open = False
    for i, ch in enumerate(s):
        o = ord(ch)
        if ch == "\r":
            if i + 1 < len(s) and s[i + 1] == "\n":
                continue
            o = 0x0A
        elif ch == "\t":
            o = 0x20
        elif o == r.zwj:
            after_zwj = True
            continue
        elif (o < 0x20 and o != 0x0A) or o == 0x7F or 0x80 <= o <= 0x9F or r.is_removed(o):
            continue
        if after_zwj and r.is_emoji(o):
            after_zwj = False
            continue
        after_zwj = False
        if r.ri_lo <= o <= r.ri_hi:
            if ri_open:
                ri_open = False
                continue
            ri_open = True
            o = r.flag
        else:
            ri_open = False
        cps.append(o)

    # 3: clusters, mark cap, composition
    seq: list[int] = []
    base: int | None = None
    marks: list[int] = []
    started = False

    def flush() -> None:
        if not started:
            return
        if base is None:
            seq.extend(marks)
        else:
            b, left = compose(r, base, marks)
            seq.append(b)
            seq.extend(left)

    for cp in cps:
        if r.is_mark(cp):
            if not started:
                started, base, marks = True, None, []
            if len(marks) < r.max_marks:
                marks.append(cp)
        else:
            flush()
            started, base, marks = True, cp, []
    flush()

    # 4: renderability, placeholder runs
    out: list[int] = []
    in_run = False
    prev_renderable = False
    for cp in seq:
        if r.is_mark(cp) and prev_renderable:
            continue
        if not r.is_mark(cp) and r.renderable(cp):
            out.append(cp)
            prev_renderable, in_run = True, False
            continue
        if not in_run:
            out.append(r.placeholder)
            in_run = True
        prev_renderable = False
    text = "".join(map(chr, out))

    # 5: truncation
    if budget is not None and len(text.encode()) > budget:
        ell = chr(r.ellipsis)
        room = budget - len(ell.encode()) if budget >= len(ell.encode()) else budget
        cut, used = [], 0
        for ch in text:
            n = len(ch.encode())
            if used + n > room:
                break
            cut.append(ch)
            used += n
        text = "".join(cut) + (ell if budget >= len(ell.encode()) else "")
    return text
