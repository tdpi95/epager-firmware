"""ePager test client: scripted flows and a benchmark against a board over TCP or BLE.

    tools/.venv/bin/python tools/test_client --tcp 127.0.0.1:47800 run [FLOW ...]
    tools/.venv/bin/python tools/test_client --ble AA:BB:CC:DD:EE:FF [--passkey 123456] bench
    tools/.venv/bin/python tools/test_client --scan
    tools/.venv/bin/python tools/test_client --self-test     (in-process fake board, every flow)
"""
import argparse
import asyncio
import json
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    __package__ = "test_client"

from test_client.bench import bench  # noqa: E402
from test_client.client import EpagerClient  # noqa: E402
from test_client.flows import FLOWS, Session, run_flows  # noqa: E402
from test_client.links import BleLink, TcpLink  # noqa: E402


async def amain(args: argparse.Namespace) -> int:
    if args.scan:
        for addr, name in await BleLink.scan():
            print(addr, name)
        return 0
    board = server = None
    if args.self_test:
        from fake_board.board import BoardConfig, FakeBoard
        from fake_board.tcp import TcpServer
        board = FakeBoard(BoardConfig(), clock="manual")
        server = TcpServer(board, port=0, log=lambda _s: None)
        await server.start()
        args.tcp = f"127.0.0.1:{server.port}"

    async def connect() -> EpagerClient:
        if args.ble:
            return EpagerClient(await BleLink(args.ble, args.passkey, not args.no_pair).connect())
        host, _, port = (args.tcp or "127.0.0.1:47800").rpartition(":")
        return EpagerClient(await TcpLink(host or "127.0.0.1", int(port)).connect())

    s = Session(connect, board, log=print)
    try:
        await s.open()
        if args.cmd == "bench":
            print(json.dumps(await bench(s.client, upload_bytes=args.bytes), indent=1))
            return 0
        names = args.flows or [n for n, (_f, needs) in FLOWS.items() if board is not None or not needs]
        results = await run_flows(s, names)
        failed = [n for n, r in results.items() if r.startswith("FAILED")]
        print(f"{len(results) - len(failed)}/{len(results)} flows ok" + (f"; failed: {', '.join(failed)}" if failed else ""))
        return 1 if failed else 0
    finally:
        await s.close()
        if server:
            await server.stop()


def main() -> int:
    ap = argparse.ArgumentParser(prog="test_client", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tcp", metavar="HOST:PORT", help="fake board over TCP (default 127.0.0.1:47800)")
    ap.add_argument("--ble", metavar="ADDRESS", help="board over BLE (bleak)")
    ap.add_argument("--passkey", type=int, metavar="NNNNNN", help="the code the board shows (default: ask when needed)")
    ap.add_argument("--no-pair", action="store_true", help="BLE: do not pair (this laptop is bonded already)")
    ap.add_argument("--scan", action="store_true", help="list ePager boards advertising nearby")
    ap.add_argument("--self-test", action="store_true", help="start an in-process fake board and run every flow")
    ap.add_argument("cmd", nargs="?", choices=["run", "bench", "list"], default="run")
    ap.add_argument("flows", nargs="*", help=f"flows to run (default: all that apply): {', '.join(FLOWS)}")
    ap.add_argument("--bytes", type=int, default=64 * 1024, help="bench upload size")
    args = ap.parse_args()
    if args.cmd == "list":
        for n, (_f, needs) in FLOWS.items():
            print(f"{n:24} {'(needs the in-process fake board)' if needs else ''}")
        return 0
    return asyncio.run(amain(args))


if __name__ == "__main__":
    sys.exit(main())
