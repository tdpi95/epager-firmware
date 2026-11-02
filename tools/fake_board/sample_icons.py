"""Sample app icons (24x24 rows of '#' black / '.' white) for scenes and the console. Stand-ins for
what the companion app renders from an Android small icon."""
from __future__ import annotations

SIZE = 24


def _grid() -> list[list[str]]:
    return [["."] * SIZE for _ in range(SIZE)]


def _rows(g: list[list[str]]) -> list[str]:
    return ["".join(r) for r in g]


def _fill(g: list[list[str]], x0: int, y0: int, x1: int, y1: int, c: str = "#") -> None:
    for y in range(y0, y1):
        for x in range(x0, x1):
            g[y][x] = c


def _line(g: list[list[str]], x0: int, y0: int, x1: int, y1: int, w: int = 3, c: str = "#") -> None:
    n = max(abs(x1 - x0), abs(y1 - y0)) or 1
    for i in range(n + 1):
        x, y = x0 + (x1 - x0) * i // n, y0 + (y1 - y0) * i // n
        _fill(g, max(0, x - w // 2), max(0, y - w // 2), min(SIZE, x - w // 2 + w), min(SIZE, y - w // 2 + w), c)


def zalo() -> list[str]:
    """A white Z on a black rounded square."""
    g = _grid()
    _fill(g, 1, 1, 23, 23)
    for x, y in ((1, 1), (22, 1), (1, 22), (22, 22)):
        g[y][x] = "."
    _fill(g, 6, 6, 18, 8, ".")
    _fill(g, 6, 16, 18, 18, ".")
    _line(g, 17, 8, 7, 16, 2, ".")
    return _rows(g)


def ring() -> list[str]:
    """A ring with a dot."""
    g = _grid()
    for y in range(SIZE):
        for x in range(SIZE):
            d2 = (x - 11.5) ** 2 + (y - 11.5) ** 2
            if 6.5 ** 2 <= d2 <= 11 ** 2 or d2 <= 3 ** 2:
                g[y][x] = "#"
    return _rows(g)


def cross() -> list[str]:
    g = _grid()
    _line(g, 4, 4, 19, 19, 4)
    _line(g, 19, 4, 4, 19, 4)
    return _rows(g)


SAMPLES = {"zalo": zalo, "ring": ring, "cross": cross}


def zalo_colour(size: int = 32) -> list[list[tuple[int, int, int, int]]]:
    """A colour stand-in for a launcher icon (LCD, `rgb565a8`): a white Z on a blue rounded square,
    transparent corners with a half-transparent edge."""
    r = size // 4
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            cx = min(max(x, r), size - 1 - r)
            cy = min(max(y, r), size - 1 - r)
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            a = 255 if d2 <= (r - 1) ** 2 else 128 if d2 <= r ** 2 else 0
            z = (8 <= y < 12 or 20 <= y < 24) and 8 <= x < 24 or 12 <= y < 20 and abs((x - 8) - (20 - y) * 2) <= 1
            row.append((255, 255, 255, a) if z and a else (0, 104, 255, a) if a else (0, 0, 0, 0))
        rows.append(row)
    return rows
