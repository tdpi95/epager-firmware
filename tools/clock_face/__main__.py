"""epager-face (M8a: build and check; the rest of the tool comes with M8c).

    python -m clock_face build FACE_DIR --board paper|lcd [-o FILE]
    python -m clock_face check FILE --board paper|lcd [--id ID]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from clock_face import build as B  # noqa: E402
from epager_proto import clockface as CF  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="epager-face", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("face_dir", type=Path)
    b.add_argument("--board", required=True, choices=sorted(CF.LIMITS))
    b.add_argument("-o", "--out", type=Path)
    c = sub.add_parser("check")
    c.add_argument("file", type=Path)
    c.add_argument("--board", required=True, choices=sorted(CF.LIMITS))
    c.add_argument("--id")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "build":
            data = B.build_file(args.face_dir, args.board)
            out = args.out or args.face_dir / f"{args.face_dir.name}-{args.board}.epface"
            out.write_bytes(data)
            print(f"{out}: {len(data)} bytes (limit {CF.LIMITS[args.board].max_file})")
        else:
            data = args.file.read_bytes()
            f = CF.parse_face(data, CF.LIMITS[args.board], args.id)
            print(f"{args.file}: ok, {f.meta['id']} {f.meta['version']}, {len(f.images)} images, "
                  f"{len(f.options)} options, {len(f.elements)} elements, {len(data)} bytes")
    except (B.SourceError, CF.FaceError, OSError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
