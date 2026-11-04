"""Clock face files for scenes, tests and the console: the built-in faces' sources rebuilt as if somebody had
installed them (so the firmware's badge is not drawn), and a damaged file."""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from clock_face import build as B  # noqa: E402
from epager_proto import clockface as CF  # noqa: E402
from epager_proto import transfer  # noqa: E402

FACES = Path(__file__).resolve().parents[2] / "components" / "ui_common" / "faces"
COUNT = {  # where a face that wants the count of waiting notifications puts it
    "paper": {"kind": "text", "x": 148, "y": 108, "w": 40, "h": 20, "font": "small_bold", "align": "right",
              "source": "notif_count", "visible_if": [{"above_zero": "notif_count"}]},
    "lcd": {"kind": "text", "x": 236, "y": 116, "w": 48, "h": 28, "font": "small_bold", "align": "right",
            "source": "notif_count", "colour": "#f59e0b", "visible_if": [{"above_zero": "notif_count"}]},
}
SOURCE_ELEMENT_FOR_UPDATE = {"kind": "text", "x": 0, "y": 0, "w": 20, "h": 20, "font": "small", "literal": "!",
                             "visible_if": [{"has": "update_available"}, {"above_zero": "update_available"}]}


def big_variant(board: str, ident: str, count: bool = False, draws_update: bool = False) -> bytes:
    """The face "Big" under another id; with the notification count as a field it places itself, and/or with
    an element that draws update_available (then the firmware adds no update mark)."""
    src = json.loads((FACES / "big" / "face.json").read_text())
    src["meta"]["id"] = ident
    src["meta"]["name"] = f"Sample {ident}"
    els = src["boards"][board]["elements"]
    if count:
        els.append(dict(COUNT[board]))
    if draws_update:
        els.append(dict(SOURCE_ELEMENT_FOR_UPDATE, x=2 if board == "paper" else 8, y=2))
    return B.build(src, FACES / "big", board)


def broken(board: str) -> bytes:
    """A file with a good container and a description that is not one: the transfer accepts it (the header is
    consistent), the parser does not."""
    lim = CF.LIMITS[board]
    payload = struct.pack("<I", 5) + b"\x01\xff\xff\x00\x00" + b"\x00\x00\x00"
    return transfer.container(CF.ASSET_TYPE_CLOCK_FACE, CF.FORMAT_CLOCK_FACE_V1, payload, lim.width, lim.height)
