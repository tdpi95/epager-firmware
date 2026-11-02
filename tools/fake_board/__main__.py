"""ePager fake board: the board core in a process, over TCP (protocol.md 10) and/or BLE, with an
optional simulated screen: the Paper (default) or, with --board lcd, the LCD with touch.
See tools/README.md.

    tools/.venv/bin/python tools/fake_board [serve] [--board lcd] [--view] [--ble] [--no-screen] ...
    tools/.venv/bin/python tools/fake_board snapshot OUT.png [--board lcd] (--scene NAME | --post TITLE [--body BODY])
"""
import argparse
import asyncio
import sys
import threading
from pathlib import Path

if __package__ in (None, ""):  # run as `python tools/fake_board`
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    __package__ = "fake_board"

from fake_board import png, scenes, scenes_lcd  # noqa: E402
from fake_board.board import BoardConfig, FakeBoard, describe  # noqa: E402
from fake_board.console import HELP, run_command  # noqa: E402
from fake_board.tcp import DEFAULT_PORT, TcpServer  # noqa: E402


def serve(args: argparse.Namespace) -> int:
    log = (lambda _s: None) if args.quiet else (lambda s: print(s, flush=True))
    storage = args.storage or (8257536 if args.board == "lcd" else 1966080)  # the boards' assets partitions
    cfg = BoardConfig(screen=not args.no_screen, board=args.board, storage_total=storage, version=args.version,
                      allow_downgrade=args.allow_downgrade, device_name=args.name, secure=not args.insecure,
                      mtu=args.mtu)
    board = FakeBoard(cfg, clock="real", on_event=lambda e: log(describe(e)))
    if args.ghosting:
        board.set_ghosting(True)
    board.start()
    stop = threading.Event()
    started = threading.Event()
    failure: list[BaseException] = []

    async def transports() -> None:
        servers = []
        try:
            if not args.no_tcp:
                tcp = TcpServer(board, args.host, args.port, log)
                await tcp.start()
                servers.append(tcp)
                log(f"[tcp] listening on {args.host}:{tcp.port}")
            if args.ble:
                from fake_board.ble import BleServer
                ble = BleServer(board, mtu=args.ble_mtu, secure=not args.insecure, log=log)
                await ble.start()
                servers.append(ble)
        except BaseException as e:  # report and stop
            failure.append(e)
            stop.set()
        started.set()
        while not stop.is_set():
            await asyncio.sleep(0.2)
        for s in servers:
            await s.stop()

    loop = asyncio.new_event_loop()
    net = threading.Thread(target=lambda: loop.run_until_complete(transports()), name="transports", daemon=True)
    net.start()
    started.wait(30)
    if failure:
        print(f"error: {failure[0]}", file=sys.stderr)
        board.stop()
        return 1
    log(f"fake board {cfg.device_name}, firmware {cfg.version}, "
        f"{'screen' if board.screen else 'no screen'}; type 'help' for commands")

    def console() -> None:
        for line in sys.stdin:
            if not run_command(board, line, log if not args.quiet else print):
                break
        else:
            if not args.view:
                return  # stdin closed (e.g. started in the background): keep serving
        stop.set()

    if not args.no_console:
        threading.Thread(target=console, name="console", daemon=True).start()
    try:
        if args.view:
            from fake_board.viewer import Viewer
            Viewer(board, zoom=args.zoom, on_close=stop.set).run()
        else:
            while not stop.wait(0.5):
                pass
    except KeyboardInterrupt:
        pass
    stop.set()
    net.join(5)
    board.stop()
    return 0


def snapshot(args: argparse.Namespace) -> int:
    mod = scenes_lcd if args.board == "lcd" else scenes
    w, h, ch = (scenes_lcd.WIDTH, scenes_lcd.HEIGHT, 3) if args.board == "lcd" else (200, 200, 1)
    if args.scene:
        if args.scene not in mod.SCENES:
            print(f"error: no scene {args.scene!r} for {args.board}: {', '.join(mod.SCENES)}", file=sys.stderr)
            return 2
        frames = mod.SCENES[args.scene]()
        if len(frames) == 1:
            png.write(args.out, frames[0][1], w, h, args.zoom, ch)
            print(f"wrote {args.out}")
        else:
            out = Path(args.out)
            for name, px in frames:
                path = out.with_name(f"{out.stem}-{name}{out.suffix}")
                png.write(path, px, w, h, args.zoom, ch)
                print(f"wrote {path}")
        return 0
    b = mod.board()
    for i, title in enumerate(args.post or []):
        scenes.post(b, i, title, args.body or "", app=args.app)
    mod.settle(b)
    px = scenes_lcd.rgb(b) if args.board == "lcd" else b.panel()
    png.write(args.out, px, w, h, args.zoom, ch)
    print(f"wrote {args.out}")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="fake_board", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=HELP)
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("serve", help="serve over TCP and/or BLE (default)")
    s.add_argument("--host", default="127.0.0.1", help="TCP bind address (0.0.0.0 for a phone on the LAN)")
    s.add_argument("--port", type=int, default=DEFAULT_PORT)
    s.add_argument("--no-tcp", action="store_true")
    s.add_argument("--ble", action="store_true", help="BLE peripheral on this machine (BlueZ, via bless)")
    s.add_argument("--ble-mtu", type=int, default=None, help="force the BLE MTU instead of BlueZ's value")
    s.add_argument("--mtu", type=int, default=517, help="TCP link MTU (lower it to exercise segmentation)")
    s.add_argument("--board", choices=("paper", "lcd"), default="paper", help="which board to simulate")
    s.add_argument("--no-screen", action="store_true", help="libepager_core.so: no LVGL, no simulator")
    s.add_argument("--view", action="store_true", help="Tk window with the simulated panel")
    s.add_argument("--zoom", type=int, default=3)
    s.add_argument("--ghosting", action="store_true")
    s.add_argument("--insecure", action="store_true", help="link not encrypted/bonded: INSUFFICIENT_SECURITY")
    s.add_argument("--version", default="0.1.0", help="running firmware version")
    s.add_argument("--allow-downgrade", action="store_true")
    s.add_argument("--storage", type=int, default=0, help="asset storage bytes (default: the board's partition)")
    s.add_argument("--name", default="ePager-FA4E")
    s.add_argument("--no-console", action="store_true")
    s.add_argument("--quiet", action="store_true")
    p = sub.add_parser("snapshot", help="render the screen headless to a PNG")
    p.add_argument("out")
    p.add_argument("--board", choices=("paper", "lcd"), default="paper")
    p.add_argument("--scene", help=f"Paper: {', '.join(scenes.SCENES)}; LCD: {', '.join(scenes_lcd.SCENES)}")
    p.add_argument("--post", action="append", help="notification title (repeatable)")
    p.add_argument("--body")
    p.add_argument("--app", default="Zalo")
    p.add_argument("--zoom", type=int, default=2)
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0].startswith("-") and argv[0] not in ("-h", "--help"):
        argv = ["serve", *argv]
    args = ap.parse_args(argv)
    return snapshot(args) if args.cmd == "snapshot" else serve(args)


if __name__ == "__main__":
    sys.exit(main())
